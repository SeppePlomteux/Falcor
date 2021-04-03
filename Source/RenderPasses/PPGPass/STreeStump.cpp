#include "STreeStump.h"

bool STreeStumpNode::isLeaf()
{
    static const uint2 zero = uint2(0, 0);
    return mChildren == zero;
}

bool STreeStumpNode::canBeMerged(STreeStump& tree, float tresHold)
{
    if (isLeaf())
        return false;
    bool res = true;
    res &= tree.getNodeAt(mChildren.x).isLeaf();
    res &= tree.getNodeAt(mChildren.y).isLeaf();
    res &= mStatisticalWeight <= tresHold;

    return res;
}

MyAABB::MyAABB(float3 min, float3 max)
{
    mMin = min;
    mMax = max;
}

inline float3 MyAABB::getExtents() const
{
    return mMax - mMin;
}

bool STreeStumpNode::canBeSplit(STreeStump& tree, float tresHold)
{
    if (!isLeaf())
        return false;
    return mStatisticalWeight >= tresHold;
}

STreeStump::STreeStump(float2 tresHolds, float factor, MyAABB aabb) : mSplitTresHold(tresHolds.x), mMergeTresHold(tresHolds.y), mAABB(aabb)
{
    mNodes.emplace_back(STreeStumpNode());
    mNewDTreeIndex = 1;
    mNewSTreeIndex = 1;
    mStatWeightFactor = factor;
    mNodes[0].mDTreeIndex = 0;
    mNodes[0].mStatisticalWeight = 0.f;
    mNodes[0].mChildren = uint2(0, 0);
    mNodes[0].mAxis = 0;
}

uint STreeStump::getEstimatedSTreeSize() const
{
    return mNewSTreeIndex;
}

uint STreeStump::getEstimatedAmountOfDTrees() const
{
    return mNewDTreeIndex;
}

const MyAABB& STreeStump::getAABB() const
{
    return mAABB;
}

uint STreeStump::getNewDTreeIndex()
{
    if (!mFreeDTreeIndices.empty())
    {
        uint res = mFreeDTreeIndices.back();
        mFreeDTreeIndices.pop_back();
        return res;
    }
    return mNewDTreeIndex++;
}

uint STreeStump::getNewSTreeIndex()
{
    if (!mFreeSTreeIndices.empty())
    {
        uint res = mFreeSTreeIndices.back();
        mFreeSTreeIndices.pop_back();
        return res;
    }
    mNodes.emplace_back(); // create new node
    return mNewSTreeIndex++;
}

STreeStumpNode& STreeStump::getNodeAt(uint index)
{
    return mNodes[index];
}

inline float generateRandomFloat()
{
    return static_cast<float>(std::rand()) / (RAND_MAX + 1);
}

void STreeStump::splitNode(STreeChangeData& res, const uint highIndex)
{
    res.mDTreeCopyData.mCopySource = mNodes[highIndex].mDTreeIndex;
    for (int i = 0; i < 2; i++)
    {
        uint newIndex = getNewSTreeIndex();
        mNodes[highIndex].mChildren[i] = newIndex;

        mNodes[newIndex].mStatisticalWeight = mNodes[highIndex].mStatisticalWeight / 2;
        mNodes[newIndex].mChildren = uint2(0, 0);
        mNodes[newIndex].mAxis = (mNodes[highIndex].mAxis + 1) % 3;
        if (i)
        {
            mNodes[newIndex].mDTreeIndex = getNewDTreeIndex();
            res.mDTreeCopyData.mCopyDest = mNodes[newIndex].mDTreeIndex;
        }
        else
        {
            mNodes[newIndex].mDTreeIndex = mNodes[highIndex].mDTreeIndex;
        }
    }
    mNodes[highIndex].mDTreeIndex = 0;

    res.mSecondChangedNode.mIndex = highIndex;
    res.mSecondChangedNode.mDTreeIndex = 0;
    res.mSecondChangedNode.mChildren = mNodes[highIndex].mChildren;

    STreeStumpNode& newNode = mNodes[mNodes[highIndex].mChildren.x];
    res.mFirstNewNode.mIndex = mNodes[highIndex].mChildren.x;
    res.mFirstNewNode.mData = uint4(newNode.mDTreeIndex, newNode.mAxis, newNode.mChildren);

    STreeStumpNode& seccondNewNode = mNodes[mNodes[highIndex].mChildren.y];
    res.mSecondNewNode.mIndex = mNodes[highIndex].mChildren.y;
    res.mSecondNewNode.mData = uint4(seccondNewNode.mDTreeIndex, seccondNewNode.mAxis, seccondNewNode.mChildren);
}

STreeChangeData STreeStump::updateTree()
{
    STreeChangeData res = { };
    res.mFirstNewNode.mIndex = std::numeric_limits<uint>::max();
    res.mSecondNewNode.mIndex = std::numeric_limits<uint>::max();
    res.mFirstChangedNode.mIndex = std::numeric_limits<uint>::max();
    res.mSecondChangedNode.mIndex = std::numeric_limits<uint>::max();
    uint highIndex = 0, lowIndex = 0;

    highIndex = lowIndex = 0; // Set both nodes to root

    while (!mNodes[lowIndex].isLeaf())
    {
        auto& currNode = mNodes[lowIndex];
        float tresHold = 0;
        float SW1 = mNodes[currNode.mChildren[0]].mStatisticalWeight, SW2 = mNodes[currNode.mChildren[1]].mStatisticalWeight;
        if (SW1 + SW2 == 0)
            tresHold = .5f;
        else if (SW1 * SW2 == 0)
            tresHold = SW1 == 0 ? 1.f : 0.f;
        else
            tresHold = 1.f / SW1 / (1.f / SW1 + 1.f / SW2);

        if (generateRandomFloat() < tresHold)
            lowIndex = currNode.mChildren[0];
        else
            lowIndex = currNode.mChildren[1];

        if (mNodes[lowIndex].canBeMerged(*this, 2000.f))
        {
            mFreeDTreeIndices.emplace_back(mNodes[mNodes[lowIndex].mChildren.y].mDTreeIndex);
            mNodes[lowIndex].mDTreeIndex = mNodes[mNodes[lowIndex].mChildren.x].mDTreeIndex;
            mNodes[lowIndex].mChildren = uint2(0, 0);

            // Add freed nodes to list of free nodes
            mFreeSTreeIndices.push_back(mNodes[lowIndex].mChildren.x);
            mFreeSTreeIndices.push_back(mNodes[lowIndex].mChildren.y);
            // Set changed node
            res.mFirstChangedNode.mIndex = lowIndex;
            res.mFirstChangedNode.mChildren = uint2(0, 0);
            res.mFirstChangedNode.mDTreeIndex = mNodes[lowIndex].mDTreeIndex;

            break;
        }
    }

    if (mNodes[highIndex].canBeSplit(*this, mSplitTresHold))
    {
        splitNode(res, highIndex);
        return res;
    }

    while (!mNodes[highIndex].isLeaf())
    {
        auto& currNode = mNodes[highIndex];

        float tresHold = mNodes[currNode.mChildren[0]].mStatisticalWeight;
        tresHold = tresHold / (tresHold + mNodes[currNode.mChildren[1]].mStatisticalWeight);
        if (generateRandomFloat() < tresHold)
            highIndex = currNode.mChildren[0];
        else
            highIndex = currNode.mChildren[1];

        if (mNodes[highIndex].canBeSplit(*this, mSplitTresHold))
        {
            splitNode(res, highIndex);
            break;
        }
    }

    return res;
}

void STreeStump::multiplyStatWeight()
{
//#pragma omp parallel for shared(mNodes)
    for (auto& node : mNodes)
    {
        node.mStatisticalWeight *= mStatWeightFactor;
    }
}


void STreeStump::addToStatisticalWeight(std::vector<uint> newData)
{
    addToStatisticalWeight(newData, 0);
}


void STreeStump::addToStatisticalWeight(std::vector<uint> newData, uint currNode)
{
    if (mNodes[currNode].isLeaf())
    {
        mNodes[currNode].mStatisticalWeight += static_cast<float>(newData[mNodes[currNode].mDTreeIndex]);
    }
    else
    {
        float total = 0;
        for (int i = 0; i < 2; i++)
        {
            addToStatisticalWeight(newData, mNodes[currNode].mChildren[i]);
            total += mNodes[mNodes[currNode].mChildren[i]].mStatisticalWeight;
        }
        mNodes[currNode].mStatisticalWeight = total;
    }
}
