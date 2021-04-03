#pragma once

#include "Falcor.h"

using namespace Falcor;

class STreeStump;

class STreeStumpNode
{
public:
    bool isLeaf();

    bool canBeMerged(STreeStump& tree, float tresHold);
    bool canBeSplit(STreeStump& tree, float tresHold);
private:
    friend class STreeStump;

    uint mDTreeIndex = 0;
    float mStatisticalWeight = 0.f;
    uint2 mChildren = uint2(0, 0);
    uint mAxis = 0;
};

struct DTreeCopyData
{
    uint mCopySource;
    uint mCopyDest;
};

struct STreeChangeData
{
    struct NewNode
    {
        uint mIndex;
        uint4 mData;
    };

    struct ChangedNode
    {
        uint mIndex;
        uint mDTreeIndex;
        uint2 mChildren;
    };

    NewNode mFirstNewNode, mSecondNewNode;
    ChangedNode mFirstChangedNode, mSecondChangedNode;
    DTreeCopyData mDTreeCopyData;
};

struct AABB
{
    AABB(float3 min, float3 max);
    inline float3 getExtents() const;

    float3 mMin;
    float3 mMax;
};

class STreeStump
{
public:
    using SharedPtr = std::shared_ptr<STreeStump>;

    STreeStump(float2 tresHolds, float weightFactor, AABB aabb);

    void splitNode(STreeChangeData& res, const uint highIndex);
    /*
     * Randomly updates the spatial tree: tries to deepen out 1 node and tries to eliminate 1 set of children.
     *
     * 
     */
    STreeChangeData updateTree();

    void multiplyStatWeight();

    void addToStatisticalWeight(std::vector<uint> newData);

    uint getEstimatedSTreeSize() const;
    uint getEstimatedAmountOfDTrees() const;
    const AABB& getAABB() const;

    STreeStumpNode& getNodeAt(uint index);
private:
    void addToStatisticalWeight(std::vector<uint> newData, uint currNode);
    uint getNewDTreeIndex();
    uint getNewSTreeIndex();

    AABB mAABB;

    std::vector<STreeStumpNode> mNodes;
    std::vector<uint> mFreeDTreeIndices;
    std::vector<uint> mFreeSTreeIndices;
    uint mNewDTreeIndex;
    uint mNewSTreeIndex;
    float mSplitTresHold, mMergeTresHold;
    float mStatWeightFactor;
};
