#pragma once

#include "Falcor.h"

#include<atomic>
#include<array>

using namespace Falcor;

static void addToAtomicFloat(std::atomic<float>& varToUpdate, float valToAdd);

// Potential enum classes for configuration of trees go here (or better yet in a seperate file)

class QuadTreeNode
{
public:
    QuadTreeNode();
    QuadTreeNode(const QuadTreeNode& arg);

    QuadTreeNode& operator=(const QuadTreeNode& arg);

    void setSum(int index, float newSumValue);
    void setSum(float newSumValue);
    float sum(int index) const; /* Returns the sum value at the specified index */

    void copyFrom(const QuadTreeNode& arg);

    void setChild(int childIndex, uint16_t newChild); /* newChild points to a QuadTreeNode instance in an array */
    uint16_t child(int childIndex) const;
    int childIndex(float2& location) const;

    bool isLeaf(int index) const;

    // Evaluates the directional irradiance *sum density* (i.e. sum / area) at a given location p.
    // To obtain radiance, the sum density (result of this function) must be divided
    // by the total statistical weight of the estimates that were summed up.
    float eval(float2& p, const std::vector<QuadTreeNode>& nodes) const;
    float pdf(float2& p, const std::vector<QuadTreeNode>& nodes) const;

    int depthAt(float2& p, const std::vector<QuadTreeNode>& nodes) const;

    /* This function should be implemented on the gpu */
    float2 sample(const std::vector<QuadTreeNode>& nodes) const;

    float computeOverlappingArea(const float2& min1, const float2& max1, const float2& min2, const float2& max2);

    void record(float2& p, float irradiance, std::vector<QuadTreeNode>& nodes);
    void record(const float2& origin, float size, float2 nodeOrigin, float nodeSize, float value, std::vector<QuadTreeNode>& nodes);

    void build(std::vector<QuadTreeNode>& nodes);

private:
    friend class SDTreeTextureBuilder;
    std::array<std::atomic<float>, 4> mSums;
    std::array<uint16_t, 4> mChildIndices;
};

class DTree
{
public:
    DTree();

    const QuadTreeNode& node(size_t i) const;

    float mean() const;

    void recordIrradiance(float2 p, float irradiance, float statisticalWeight);

    float pdf(float2 p) const;

    int depthAt(float2 p) const;
    int depth() const;

    size_t numNodes() const;

    float statisticalWeight() const;
    void setStatisticalWeight(float newWeight);

    /* This function should be implemented on the gpu */
    float2 sample() const;

    void reset(const DTree& previousDTree, int newMaxDepth, float subdivisionTreshold);

    size_t approxMemoryFootprint() const;

    void build();
private:
    friend class SDTreeTextureBuilder;
    std::vector<QuadTreeNode> mNodes;

    struct Atomic
    {
        Atomic();
        Atomic(const Atomic& arg);

        Atomic& operator=(const Atomic& arg);

        std::atomic<float> mSum;
        std::atomic<float> mStatisticalWeight; // What does this member mean?

    } mAtomic;

    int mMaxDepth;
};

struct DTreeRecord
{
    float3 d;
    float radiance, product; // What is product?
    float woPdf, bsdfPdf, dTreePdf; // what is woPdf?
    float statisticalWeight;
    bool isDelta;
};

class DTreeWrapper
{
public:
    DTreeWrapper();

    static float3 canonicalToDir(float2 p);
    static float2 dirToCanonical(const float3& d);

    void record(const DTreeRecord& rec);

    void build();
    void reset(int maxDepth, float subdivisionThreshold);

    /* This function should be implemented on the gpu */
    float3 sample() const;

    float pdf(const float3& dir) const;

    float diff(const DTreeWrapper& other) const;

    int depth() const;
    size_t numNodes() const;
    float meanRadiance() const;
    float statisticalWeight() const;
    float staticticalWeightBuilding() const;
    void setStatisticalWeightBuilding(float newWeight);

    size_t approxMemoryFootprint() const;

private:
    friend class SDTreeTextureBuilder;
    DTree mBuilding;
    DTree mSampling;
};

class STreeNode
{
public:
    STreeNode();

    bool isLeaf() const;
    const DTreeWrapper* getDTree() const;
    DTreeWrapper* getDTree();
    int getAxis() const;

    int childIndex(float3& p) const;
    int nodeIndex(float3& p) const;

    DTreeWrapper* dTreeWrapper(float3& p, float3& size, std::vector<STreeNode>& nodes);
    const DTreeWrapper* dTreeWrapper() const;

    int depth(float3& p, const std::vector<STreeNode>& nodes) const;
    int depth(const std::vector<STreeNode>& nodes) const;

    void forEachLeaf(std::function<void(const DTreeWrapper*, const float3&, const float3&)> funct,
        float3 p, float3 size, const std::vector<STreeNode>& nodes) const;

    float computeOverlappingVolume(const float3& min1, const float3& max1, const float3& min2, const float3& max2);

    void record(const float3& min1, const float3& max1, float3 min2, float3 size2,
        const DTreeRecord& rec, std::vector<STreeNode>& nodes);
public: // Dees is achterlijk, fix maybe later
    bool mIsLeaf = true;
    DTreeWrapper mDTree;
    int mAxis;
    std::array<uint, 2> mChildren;
};

struct AABB_OLD
{
    AABB_OLD(float3 min, float3 max);
    inline float3 getExtents() const;

    float3 mMin;
    float3 mMax;
};

class STree
{
public:
    using SharedPtr = std::shared_ptr<STree>;
    STree(const AABB_OLD& aabb);

    const AABB_OLD& aabb() const;


    void clear();

    void subdivideAll();
    void subdivide(int nodeIndex, std::vector<STreeNode>& nodes);

    DTreeWrapper* dTreeWrapper(float3 p, float3& size);
    DTreeWrapper* dTreeWrapper(float3 p);

    void forEachDTreeWrapperConst(std::function<void(const DTreeWrapper*)> func) const;
    void forEachDTreeWrapperConstP(std::function<void(const DTreeWrapper*, const float3&, const float3&)> func) const;
    void forEachDTreeWrapperParallel(std::function<void(DTreeWrapper*)> func);

    void record(const float3& p, const float3& dTreeVoxelSize, DTreeRecord rec);

    bool shallSplit(const STreeNode& node, int depth, size_t samplesRequired);

    void refine(size_t sTreeThreshold, int maxSizeInMB);
private:
    friend class SDTreeTextureBuilder;
    std::vector<STreeNode> mNodes;
    AABB_OLD mAABB;
};

struct AllocationData
{
    ~AllocationData();
    AllocationData() = default;
    AllocationData(AllocationData& other) = delete; // no implicit copying allowed
    AllocationData(AllocationData&& other) noexcept; // moving is allowed

    float* mDTreeSumsTex;
    uint* mDTreeChildrenTex;

    uint2 mDTreeTexSize;

    uint* mSTreeTex;

    uint2 mSTreeTexSize;
};

struct DTreeTexData
{
    DTreeTexData() = default;
    DTreeTexData(DTreeTexData& other) = delete; // no implicit copying allowed (as it could be very expensive)
    DTreeTexData(DTreeTexData&& other) noexcept = default;
    ~DTreeTexData() = default;
    std::vector<float4> mDTreeSums;
    std::vector<uint> mDTreeStatisticalWeights;
    //std::vector<uint2> mDTreeChildren; // Can't change for now

    size_t mMaxDTreeSize = 0;
};

enum class DTreeType
{
    D_TREE_TYPE_SAMPLING,
    D_TREE_TYPE_BUILDING
};

class SDTreeTextureBuilder
{
public:
    static AllocationData buildSDTreeAsTextures(STree::SharedPtr pSTree, DTreeType type = DTreeType::D_TREE_TYPE_SAMPLING);
    static void updateDTreeBuilding(DTreeTexData &data, STree::SharedPtr pSTree);
};

