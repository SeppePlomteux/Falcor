#include "SDTree.h"

static void addToAtomicFloat(std::atomic<float>& varToUpdate, float valToAdd)
{
    auto current = varToUpdate.load();
    while (!varToUpdate.compare_exchange_weak(current, current + valToAdd));
}

/* ---- QuadTreeNode implementation ----*/

QuadTreeNode::QuadTreeNode()
{
    mChildIndices = {};
    for (size_t i = 0; i < mSums.size(); i++)
    {
        mSums[i].store(0, std::memory_order_relaxed);
    }
}

QuadTreeNode::QuadTreeNode(const QuadTreeNode& arg)
{
    copyFrom(arg);
}

QuadTreeNode& QuadTreeNode::operator=(const QuadTreeNode& arg)
{
    copyFrom(arg);
    return *this;
}

void QuadTreeNode::setSum(int index, float newSumValue)
{
    mSums[index].store(newSumValue, std::memory_order_relaxed);
}

void QuadTreeNode::setSum(float newSumValue)
{
    for (int i = 0; i < 4; i++)
    {
        setSum(i, newSumValue);
    }
}

/* Returns the sum value at the specified index */
float QuadTreeNode::sum(int index) const
{
    return mSums[index].load(std::memory_order_relaxed);
}

void QuadTreeNode::copyFrom(const QuadTreeNode& arg)
{
    for (int i = 0; i < 4; i++)
    {
        setSum(i, arg.sum(i));
        mChildIndices[i] = arg.mChildIndices[i];
    }
}

/* newChild points to a QuadTreeNode instance in an array */
void QuadTreeNode::setChild(int childIndex, uint16_t newChild)
{
    mChildIndices[childIndex] = newChild;
}

uint16_t QuadTreeNode::child(int childIndex) const
{
    return mChildIndices[childIndex];
}

int QuadTreeNode::childIndex(float2& location) const
{
    int res = 0;
    for (int i = 0; i < 2; i++)
    {
        if (location[i] < 0.5f)
        {
            location[i] *= 2;
        }
        else
        {
            location[i] = 2 * location[i] - 1.0f; // On GPU, this is 1 MAD instruction
            res |= 1 << i;
        }
    }
    return res;
}


bool QuadTreeNode::isLeaf(int index) const
{
    return child(index) == 0;
}

float QuadTreeNode::eval(float2& p, const std::vector<QuadTreeNode>& nodes) const
{
    assert(p.x >= 0 && p.x <= 1 && p.y >= 0 && p.y <= 1);
    const int index = childIndex(p);
    if (isLeaf(index))
    {
        return 4 * sum(index); // Geen idee wat deze 4 moet betekenen
    }
    else
    {
        return 4 * nodes[child(index)].eval(p, nodes);
    }
}

float QuadTreeNode::pdf(float2& p, const std::vector<QuadTreeNode>& nodes) const
{
    assert(p.x >= 0 && p.x <= 1 && p.y >= 0 && p.y <= 1);
    const int index = childIndex(p);
    if (!(sum(index) > 0))
    {
        return 0;
    }

    const float factor = 4 * sum(index) / (sum(0) + sum(1) + sum(2) + sum(3));
    if (isLeaf(index))
    {
        return factor;
    }
    else
    {
        return factor * nodes[child(index)].pdf(p, nodes);
    }
}

int QuadTreeNode::depthAt(float2& p, const std::vector<QuadTreeNode>& nodes) const
{
    assert(p.x >= 0 && p.x <= 1 && p.y >= 0 && p.y <= 1);
    const int index = childIndex(p);
    if (isLeaf(index))
    {
        return 1;
    }
    else
    {
        return 1 + nodes[child(index)].depthAt(p, nodes);
    }
}

/* This function should be implemented on the gpu */
float2 QuadTreeNode::sample(const std::vector<QuadTreeNode>& nodes) const
{
    throw std::exception("Not yet implemented!");
}

float QuadTreeNode::computeOverlappingArea(const float2& min1, const float2& max1, const float2& min2, const float2& max2)
{
    float lengths[2] = {};
    for (int i = 0; i < 2; i++)
    {
        lengths[i] = std::max(std::min(max1[i], max2[i]) - std::max(min1[i], min2[i]), 0.0f);
    }
    return lengths[0] * lengths[1];
}

void QuadTreeNode::record(float2& p, float irradiance, std::vector<QuadTreeNode>& nodes)
{
    assert(p.x >= 0 && p.x <= 1 && p.y >= 0 && p.y <= 1);
    int index = childIndex(p);

    if (isLeaf(index))
    {
        addToAtomicFloat(mSums[index], irradiance);
    }
    else
    {
        nodes[child(index)].record(p, irradiance, nodes);
    }
}

/*
 * Organisation of child nodes:
 * +------+------+
 * |  00  |  01  |
 * | 0b00 | 0b01 | 
 * +------+------+
 * |  02  |  03  |
 * | 0b10 | 0b11 |
 * +------+------+
 *
 * To calculate origin of child node (upper left corner),
 * half a parent node width must be added to the parent origin in some cases
 * weird if test does exactly this
 *
 */
void QuadTreeNode::record(const float2& origin, float size, float2 nodeOrigin, float nodeSize, float value, std::vector<QuadTreeNode>& nodes)
{
    float childSize = nodeSize / 2;
    for (int i = 0; i < 4; i++)
    {
        float2 childOrigin = nodeOrigin;
        if (i & 0b01)
            childOrigin.x += childSize;
        if (i & 0b10)
            childOrigin.y += childSize;

        // origin != nodeOrigin!!
        float w = computeOverlappingArea(origin, origin + float2(size), childOrigin, childOrigin + float2(childSize));
        if (w > 0.0f)
        {
            if (isLeaf(i))
            {
                // Where does this w come from? What does it mean?
                addToAtomicFloat(mSums[i], value * w);
            }
            else
            {
                nodes[child(i)].record(origin, size, childOrigin, childSize, value, nodes);
            }
        }
    }
}

void QuadTreeNode::build(std::vector<QuadTreeNode>& nodes)
{
    for (int i = 0; i < 4; i++)
    {
        // During sampling, all irradiance estimates are accumulated in
        // the leaves, so the leaves are built by definition.
        if (isLeaf(i))
        {
            continue;
        }

        QuadTreeNode& childToHandle = nodes[child(i)];

        // Build child recursivly, making it's sum field valid
        childToHandle.build(nodes);

        // Sum the child's sums to update this node's sum field
        float sum = 0;
        for (int j = 0; j < 4; j++)
        {
            sum += childToHandle.sum(j);
        }
        setSum(i, sum);
    }
}

/* ---- DTree implementation ---- */

DTree::DTree()
{
    mAtomic.mSum.store(0, std::memory_order_relaxed);
    mMaxDepth = 0;
    mNodes.emplace_back();
    mNodes.front().setSum(0.0f);
}

const QuadTreeNode& DTree::node(size_t i) const
{
    return mNodes[i];
}

float DTree::mean() const
{
    if (mAtomic.mStatisticalWeight == 0)
    {
        return 0;
    }
    const float factor = 1 / (static_cast<float>(M_PI) * 4 * mAtomic.mStatisticalWeight);
    return factor * mAtomic.mSum;
}

void DTree::recordIrradiance(float2 p, float irradiance, float statisticalWeight)
{
    if (std::isfinite(statisticalWeight) && statisticalWeight > 0)
    {
        addToAtomicFloat(mAtomic.mStatisticalWeight, statisticalWeight);

        if (std::isfinite(irradiance) && irradiance > 0)
        {
            mNodes[0].record(p, irradiance * statisticalWeight, mNodes);
        }
    }
}

float DTree::pdf(float2 p) const
{
    if (!(mean() > 0))
    {
        return 1 / (4 * static_cast<float>(M_PI));
    }

    return mNodes[0].pdf(p, mNodes) / (4 * static_cast<float>(M_PI));
}

int DTree::depthAt(float2 p) const
{
    return mNodes[0].depthAt(p, mNodes);
}

int DTree::depth() const
{
    return mMaxDepth;
}

size_t DTree::numNodes() const
{
    return mNodes.size();
}

float DTree::statisticalWeight() const
{
    return mAtomic.mStatisticalWeight;
}

void DTree::setStatisticalWeight(float newWeight)
{
    mAtomic.mStatisticalWeight = newWeight;
}

/* This function should be implemented on the gpu */
float2 DTree::sample() const
{
    throw std::exception("Not yet implemented");
}

void DTree::reset(const DTree& previousDTree, int newMaxDepth, float subdivisionThreshold)
{
    mAtomic = Atomic{};
    mMaxDepth = 0;
    mNodes.clear();
    mNodes.emplace_back();

    struct StackNode
    {
        size_t nodeIndex;
        size_t otherNodeIndex;
        const DTree* otherDTree;
        int depth;
    };

    std::stack<StackNode> nodeIndices;
    nodeIndices.push({ 0, 0, &previousDTree, 1 });

    const float total = previousDTree.mAtomic.mSum;

    // Create the topology of the new DTree to be the refined version
    // of the previous DTree. Subdivision is recursive if enough energy is there.
    while (!nodeIndices.empty())
    {
        StackNode sNode = nodeIndices.top();
        nodeIndices.pop();

        mMaxDepth = std::max(mMaxDepth, sNode.depth);

        for (int i = 0; i < 4; ++i)
        {
            const QuadTreeNode& otherNode = sNode.otherDTree->mNodes[sNode.otherNodeIndex];
            const float fraction = total > 0 ? (otherNode.sum(i) / total) : std::pow(0.25f, static_cast<float>(sNode.depth));
            assert(fraction <= 1.0f + 1e-4);
            
            if (sNode.depth < newMaxDepth && fraction > subdivisionThreshold)
            {
                if (!otherNode.isLeaf(i))
                {
                    assert(sNode.otherDTree == &previousDTree);
                    nodeIndices.push({ mNodes.size(), otherNode.child(i), &previousDTree, sNode.depth + 1 });
                }
                else
                {
                    nodeIndices.push({ mNodes.size(), mNodes.size(), this, sNode.depth + 1 });
                }

                mNodes[sNode.nodeIndex].setChild(i, static_cast<uint16_t>(mNodes.size()));
                mNodes.emplace_back();
                mNodes.back().setSum(otherNode.sum(i) / 4);

                if (mNodes.size() > std::numeric_limits<uint16_t>::max())
                {
                    // TODO log warning
                    //SLog(EWarn, "DTreeWrapper hit maximum children count.");
                    nodeIndices = std::stack<StackNode>();
                    break;
                }
            }
        }
    }

    // Uncomment once memory becomes an issue.
    // just removes unused but allocated space in mNodes
    // mNodes.shrink_to_fit();

    for (auto& node : mNodes)
    {
        node.setSum(0);
    }
}

size_t DTree::approxMemoryFootprint() const
{
    return mNodes.capacity() * sizeof(QuadTreeNode) + sizeof(*this);
}

void DTree::build()
{
    auto& root = mNodes[0];

    // Build the quadtree, starting from the root
    root.build(mNodes);

    // Make sure that the sum member is valid
    float sum = 0;
    for (int i = 0; i < 4; i++)
    {
        sum += root.sum(i);
    }
    mAtomic.mSum.store(sum);
}

/* -- DTree::Atomic implementation -- */

DTree::Atomic::Atomic()
{
    mSum.store(0, std::memory_order_relaxed);
    mStatisticalWeight.store(0, std::memory_order_relaxed);
}

DTree::Atomic::Atomic(const Atomic& arg)
{
    *this = arg;
}

DTree::Atomic& DTree::Atomic::operator=(const Atomic& arg)
{
    mSum.store(arg.mSum.load(std::memory_order_relaxed), std::memory_order_relaxed);
    mStatisticalWeight.store(arg.mStatisticalWeight.load(std::memory_order_relaxed), std::memory_order_relaxed);
    return *this;
}

/* ---- DTreeWrapper implementation ----*/

DTreeWrapper::DTreeWrapper()
{}

float3 DTreeWrapper::canonicalToDir(float2 p)
{
    const float cosTheta = 2 * p.x - 1;
    const float phi = 2 * static_cast<float>(M_PI) * p.y;

    const float sinTheta = sqrt(1 - cosTheta * cosTheta);
    float sinPhi = sin(phi);
    float cosPhi = cos(phi);

    return float3(sinTheta * cosPhi, sinTheta * sinPhi, cosTheta);
}
float2 DTreeWrapper::dirToCanonical(const float3& d)
{
    if (!std::isfinite(d.x) || !std::isfinite(d.y) || !std::isfinite(d.z))
    {
        return float2(0.f, 0.f);
    }

    const float cosTheta = std::min(std::max(d.z, -1.0f), 1.0f);
    float phi = std::atan2(d.y, d.x);
    while (phi < 0)
        phi += 2.0 * static_cast<float>(M_PI);

    return float2((cosTheta + 1) / 2, phi / (2 * static_cast<float>(M_PI)));
}

void DTreeWrapper::record(const DTreeRecord& rec)
{
    if (!rec.isDelta)
    {
        float irradiance = rec.radiance / rec.woPdf; // What? How?
        mBuilding.recordIrradiance(dirToCanonical(rec.d), irradiance, rec.statisticalWeight);
    }
}

void DTreeWrapper::build()
{
    mBuilding.build();
    mSampling = mBuilding;
}

void DTreeWrapper::reset(int maxDepth, float subdivisionThreshold)
{
    mBuilding.reset(mSampling, maxDepth, subdivisionThreshold);
}

/* This function should be implemented on the gpu */
float3 DTreeWrapper::sample() const
{
    // Always throws exception
    return canonicalToDir(mSampling.sample());
}

float DTreeWrapper::pdf(const float3& dir) const
{
    return mSampling.pdf(dirToCanonical(dir));
}

float DTreeWrapper::diff(const DTreeWrapper& other) const
{
    return 0.f;
}

int DTreeWrapper::depth() const
{
    return mSampling.depth();
}

size_t DTreeWrapper::numNodes() const
{
    return mSampling.numNodes();
}

// Function name implies radiance, but implementation implies irradiance
float DTreeWrapper::meanRadiance() const
{
    return mSampling.mean();
}

float DTreeWrapper::statisticalWeight() const
{
    return mSampling.statisticalWeight();
}

float DTreeWrapper::staticticalWeightBuilding() const
{
    return mBuilding.statisticalWeight();
}

void DTreeWrapper::setStatisticalWeightBuilding(float newWeight)
{
    mBuilding.setStatisticalWeight(newWeight);
}

size_t DTreeWrapper::approxMemoryFootprint() const
{
    return mBuilding.approxMemoryFootprint() + mSampling.approxMemoryFootprint();
}

/* ---- STreeNode implementation ---- */

STreeNode::STreeNode()
{
    mChildren = {};
    mIsLeaf = true;
    mAxis = 0;
}

bool STreeNode::isLeaf() const
{
    return mIsLeaf;
}

const DTreeWrapper* STreeNode::getDTree() const
{
    return &mDTree;
}

DTreeWrapper* STreeNode::getDTree()
{
    return &mDTree;
}

int STreeNode::getAxis() const
{
    return mAxis;
}

int STreeNode::childIndex(float3& p) const
{
    if (p[mAxis] < 0.5f)
    {
        p[mAxis] *= 2;
        return 0;
    }
    else
    {
        p[mAxis] = 2 * p[mAxis] - 1;
        return 1;
    }
}

int STreeNode::nodeIndex(float3& p) const
{
    return mChildren[childIndex(p)];
}

DTreeWrapper* STreeNode::dTreeWrapper(float3& p, float3& size, std::vector<STreeNode>& nodes)
{
    assert(p[mAxis] >= 0 && p[mAxis] <= 1);
    if (mIsLeaf)
    {
        return &mDTree;
    }
    else
    {
        size[mAxis] /= 2;
        return nodes[nodeIndex(p)].dTreeWrapper(p, size, nodes);
    }
}

const DTreeWrapper* STreeNode::dTreeWrapper() const
{
    return &mDTree;
}

int STreeNode::depth(float3& p, const std::vector<STreeNode>& nodes) const
{
    assert(p[mAxis] >= 0 && p[mAxis] <= 1);
    if (mIsLeaf)
    {
        return 1;
    }
    else
    {
        return 1 + nodes[nodeIndex(p)].depth(p, nodes);
    }
}
int STreeNode::depth(const std::vector<STreeNode>& nodes) const
{
    int result = 1;

    if (!mIsLeaf)
    {
        for (auto c : mChildren)
        {
            result = std::max(result, 1 + nodes[c].depth(nodes));
        }
    }

    return result;
}

void STreeNode::forEachLeaf(std::function<void(const DTreeWrapper*, const float3&, const float3&)> funct,
    float3 p, float3 size, const std::vector<STreeNode>& nodes) const
{
    if (mIsLeaf)
    {
        funct(&mDTree, p, size);
    }
    else
    {
        size[mAxis] /= 2;
        for (int i = 0; i < 2; i++)
        {
            float3 childPoint = p;
            if (i == 1)
            {
                childPoint[mAxis] += size[mAxis];
            }

            nodes[mChildren[i]].forEachLeaf(funct, childPoint, size, nodes);
        }
    }
}

float STreeNode::computeOverlappingVolume(const float3& min1, const float3& max1, const float3& min2, const float3& max2)
{
    float lengths[3] = {};
    for (int i = 0; i < 3; i++)
    {
        lengths[i] = std::max(std::min(max1[i], max2[i]) - std::max(min1[i], min2[i]), 0.f);
    }
    return lengths[0] * lengths[1] * lengths[2];
}

void STreeNode::record(const float3& min1, const float3& max1, float3 min2, float3 size2,
    const DTreeRecord& rec, std::vector<STreeNode>& nodes)
{
    float w = computeOverlappingVolume(min1, max1, min2, min2 + size2);
    if (w > 0)
    {
        if (mIsLeaf)
            mDTree.record({ rec.d, rec.radiance, rec.product,
                rec.woPdf, rec.bsdfPdf, rec.dTreePdf,  rec.statisticalWeight * w, rec.isDelta });
        else
        {
            size2[mAxis] /= 2;
            for (int i = 0; i < 2; i++)
            {
                if (i & 1) // if i == 1
                {
                    min2[mAxis] += size2[mAxis];
                }

                nodes[mChildren[i]].record(min1, max1, min2, size2, rec, nodes);
            }
        }
    }
}

/* ---- AABB implementation ---- */

AABB_OLD::AABB_OLD(float3 min, float3 max)
{
    mMin = min;
    mMax = max;
}

inline float3 AABB_OLD::getExtents() const
{
    return mMax - mMin;
}

/* ---- STree implementation ----*/

STree::STree(const AABB_OLD& aabb) : mAABB(aabb)
{
    clear();

    // Enlarge AABB to turn it into a cube. This has the effect
    // of nicer hierarchical subdivisions.
    float3 size = aabb.mMax - aabb.mMin;
    float maxSize = std::max(std::max(size.x, size.y), size.z);
    mAABB.mMax = mAABB.mMin + float3(maxSize);
}

const AABB_OLD& STree::aabb() const
{
    return mAABB;
}

void STree::clear()
{
    mNodes.clear();
    mNodes.emplace_back();
}

void STree::subdivideAll()
{
    int nNodes = static_cast<int>(mNodes.size());
    for (int i = 0; i < nNodes; i++)
    {
        if (mNodes[i].isLeaf())
            subdivide(i, mNodes);
    }
}

void STree::subdivide(int nodeIndex, std::vector<STreeNode>& nodes)
{
    nodes.resize(nodes.size() + 2);

    if (nodes.size() > std::numeric_limits<uint>::max())
    {
        // TODO log
        return;
    }

    STreeNode& cur = nodes[nodeIndex];
    for (int i = 0; i < 2; i++)
    {
        uint index = static_cast<uint>(nodes.size() - 2 + i);
        cur.mChildren[i] = index;
        nodes[index].mAxis = (cur.mAxis + 1) % 3;
        nodes[index].mDTree = cur.mDTree;
        nodes[index].mDTree.setStatisticalWeightBuilding(nodes[index].mDTree.staticticalWeightBuilding() / 2);
    }
    cur.mIsLeaf = false;
    cur.mDTree = {}; // to save memory
}

DTreeWrapper* STree::dTreeWrapper(float3 p, float3& size)
{
    size = mAABB.getExtents();
    p = p - mAABB.mMin;
    p.x /= size.x;
    p.y /= size.y;
    p.z /= size.z;

    return mNodes[0].dTreeWrapper(p, size, mNodes);
}

DTreeWrapper* STree::dTreeWrapper(float3 p)
{
    float3 size;
    return dTreeWrapper(p, size);
}

void STree::forEachDTreeWrapperConst(std::function<void(const DTreeWrapper*)> func) const
{
    for (auto& node : mNodes)
    {
        if (node.isLeaf())
            func(node.getDTree());
    }
}

void STree::forEachDTreeWrapperConstP(std::function<void(const DTreeWrapper*, const float3&, const float3&)> func) const
{
    mNodes[0].forEachLeaf(func, mAABB.mMin, mAABB.getExtents(), mNodes);
}

void STree::forEachDTreeWrapperParallel(std::function<void(DTreeWrapper*)> func)
{
    int nDTreeWrappers = static_cast<int>(mNodes.size());

#pragma omp parallel for schedule(guided, 5) num_threads(15)
    for (int i = 0; i < nDTreeWrappers; i++)
    {
        if (mNodes[i].isLeaf())
            func(mNodes[i].getDTree());
    }
}

void STree::record(const float3& p, const float3& dTreeVoxelSize, DTreeRecord rec)
{
    float volume = 1;
    for (int i = 0; i < 3; i++)
    {
        volume *= dTreeVoxelSize[i];
    }

    rec.statisticalWeight /= volume;
    mNodes[0].record(p - dTreeVoxelSize * 0.5f, p + dTreeVoxelSize * 0.5f,
        mAABB.mMin, mAABB.getExtents(), rec, mNodes);
}

bool STree::shallSplit(const STreeNode& node, int depth, size_t samplesRequired)
{
    return mNodes.size() < std::numeric_limits<uint>::max() - 1 &&
        node.getDTree()->staticticalWeightBuilding() > samplesRequired;
}

void STree::refine(size_t sTreeThreshold, int maxSizeInMB)
{
    if (maxSizeInMB >= 0)
    {
        size_t approxMemoryFootprint = 0;
        for (const auto& node : mNodes)
        {
            approxMemoryFootprint += node.dTreeWrapper()->approxMemoryFootprint();
        }

        if (approxMemoryFootprint / 1000000 >= static_cast<size_t>(maxSizeInMB))
        {
            return;
        }
    }

    struct StackNode
    {
        size_t index;
        int depth;
    };

    std::stack<StackNode> nodeIndices;
    nodeIndices.push({ 0,  1 });
    while (!nodeIndices.empty())
    {
        StackNode sNode = nodeIndices.top();
        nodeIndices.pop();

        // Subdivide if needed and leaf
        if (mNodes[sNode.index].isLeaf())
        {
            if (shallSplit(mNodes[sNode.index], sNode.depth, sTreeThreshold))
            {
                subdivide((int)sNode.index, mNodes);
            }
        }

        // Add children to stack if we're not
        if (!mNodes[sNode.index].isLeaf())
        {
            const STreeNode& node = mNodes[sNode.index];
            for (int i = 0; i < 2; ++i)
            {
                nodeIndices.push({ node.mChildren[i], sNode.depth + 1 });
            }
        }
    }

    // Uncomment once memory becomes an issue.
    //m_nodes.shrink_to_fit();
}

AllocationData::~AllocationData()
{
    // Free used pointers, leave no one dangling
    free(mDTreeSumsTex);
    free(mDTreeChildrenTex);
    free(mSTreeTex);
}

AllocationData::AllocationData(AllocationData&& other) noexcept
{
    // move over the internal pointers
    mDTreeSumsTex = other.mDTreeSumsTex;
    mDTreeChildrenTex = other.mDTreeChildrenTex;
    mSTreeTex = other.mSTreeTex;

    mDTreeTexSize = other.mDTreeTexSize;
    mSTreeTexSize = other.mSTreeTexSize;

    // set other pointers to null, so resources do not get freed
    other.mDTreeSumsTex = nullptr;
    other.mDTreeChildrenTex = nullptr;
    other.mSTreeTex = nullptr;
}

AllocationData SDTreeTextureBuilder::buildSDTreeAsTextures(STree::SharedPtr pSTree, DTreeType type)
{
    AllocationData res;
    size_t amountOfSTreeNodes = std::max(pSTree->mNodes.size(), (size_t) 1);
    const size_t width = static_cast<size_t>(std::ceil(std::sqrt(amountOfSTreeNodes)));
    const size_t height = static_cast<size_t>(std::ceil((double)amountOfSTreeNodes / (double)width));
    uint* pSTreeTex = new uint[4 * width * height]();
    std::vector<DTreeWrapper*> dTrees;
    size_t index = 0;
    for (auto& node : pSTree->mNodes)
    {
        uint4 blobVal;
        if (node.isLeaf())
        {
            blobVal.x = static_cast<uint>(dTrees.size());
            dTrees.push_back(&node.mDTree);
            blobVal.z = 0;
            blobVal.w = 0;
        }
        else
        {
            blobVal.x = 0;
            blobVal.z = node.mChildren[0];
            blobVal.w = node.mChildren[1];
        }
        blobVal.y = node.mAxis;
        pSTreeTex[4 * index] = blobVal.x;
        pSTreeTex[4 * index + 1] = blobVal.y;
        pSTreeTex[4 * index + 2] = blobVal.z;
        pSTreeTex[4 * index + 3] = blobVal.w;
        index++;
    }
    size_t maxDTreeSize = 1;
    if (type == DTreeType::D_TREE_TYPE_SAMPLING)
        for (auto dTree : dTrees)
        {
            maxDTreeSize = std::max((size_t) maxDTreeSize, dTree->mSampling.mNodes.size());
        }
    else
        for (auto dTree : dTrees)
        {
            maxDTreeSize = std::max((size_t)maxDTreeSize, dTree->mBuilding.mNodes.size());
        }
    float* pDTreeSums = new float[4 * dTrees.size() * maxDTreeSize]();
    uint* pDTreeChildren = new uint[2 * dTrees.size() * maxDTreeSize]();
    size_t rowIndex = 0;
    for (auto dTree : dTrees)
    {
        size_t colIndex = 0;
        DTree &wrappedDTree = type == DTreeType::D_TREE_TYPE_SAMPLING ? dTree->mSampling : dTree->mBuilding;
        for (auto& node : wrappedDTree.mNodes)
        {
            float4 sumBlobVal;
            sumBlobVal.x = node.mSums[0];
            sumBlobVal.y = node.mSums[1];
            sumBlobVal.z = node.mSums[2];
            sumBlobVal.w = node.mSums[3];
            uint2 childBlobVal;
            childBlobVal.x = (node.mChildIndices[0] << 16) + node.mChildIndices[1];
            childBlobVal.y = (node.mChildIndices[2] << 16) + node.mChildIndices[3];

            pDTreeSums[4 * (rowIndex * maxDTreeSize + colIndex)] = sumBlobVal.x; // TODO dees is nu effe bullshit
            pDTreeSums[4 * (rowIndex * maxDTreeSize + colIndex) + 1] = sumBlobVal.y; // TODO dees is nu effe bullshit
            pDTreeSums[4 * (rowIndex * maxDTreeSize + colIndex) + 2] = sumBlobVal.z; // TODO dees is nu effe bullshit
            pDTreeSums[4 * (rowIndex * maxDTreeSize + colIndex) + 3] = sumBlobVal.w; // TODO dees is nu effe bullshit
            pDTreeChildren[2 * (rowIndex * maxDTreeSize + colIndex)] = childBlobVal.x; // TODO feiks je
            pDTreeChildren[2 * (rowIndex * maxDTreeSize + colIndex) + 1] = childBlobVal.y; // TODO feiks je
            size_t kaasTest = rowIndex * maxDTreeSize + colIndex;
            //pDTreeSums[kaasTest] = sumBlobVal;

            colIndex++;
        }
        rowIndex++;
    }
    res.mDTreeSumsTex = pDTreeSums;
    res.mDTreeChildrenTex = pDTreeChildren;
    res.mDTreeTexSize = uint2(maxDTreeSize, dTrees.size());
    res.mSTreeTex = pSTreeTex;
    res.mSTreeTexSize = uint2(width, height);
    //std::cout << "STree data: " << std::endl;
    //for (size_t i = 0; i < width * height; i++)
    //{
    //    std::cout << pSTreeTex[i].x << ", " << pSTreeTex[i].y << ", " << pSTreeTex[i].z << ", " << pSTreeTex[i].w << std::endl;
    //}
    return res;
}

void SDTreeTextureBuilder::updateDTreeBuilding(DTreeTexData& data, STree::SharedPtr pSTree)
{
    size_t dTreeIndex = 0;
    for (auto& node : pSTree->mNodes)
    {
        if (!node.isLeaf())
            continue;
        addToAtomicFloat(node.mDTree.mBuilding.mAtomic.mStatisticalWeight, static_cast<float>(data.mDTreeStatisticalWeights[dTreeIndex]));
        size_t nodeIndex = 0;
        for (auto& dNode : node.mDTree.mBuilding.mNodes)
        {
            for (int i = 0; i < 4; i++)
                dNode.mSums[i].store(data.mDTreeSums[dTreeIndex * data.mMaxDTreeSize + nodeIndex][i], std::memory_order_relaxed);
            nodeIndex++;
        }
        dTreeIndex++;
    }
}
