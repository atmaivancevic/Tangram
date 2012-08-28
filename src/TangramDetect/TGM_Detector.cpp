/*
 * =====================================================================================
 *
 *       Filename:  TGM_Detector.cpp
 *
 *    Description:  Detect SV using read pair method
 *
 *        Created:  05/14/2012 01:04:16 PM
 *       Revision:  none
 *       Compiler:  g++
 *
 *         Author:  Jiantao Wu (), 
 *   Inistitution:  Boston College
 *
 * =====================================================================================
 */

#ifndef __STDC_CONSTANT_MACROS
#define __STDC_CONSTANT_MACROS
#endif

#ifndef __STDC_LIMIT_MACROS
#define __STDC_LIMIT_MACROS
#endif

#include <limits.h>

#include "TGM_Utilities.h"
#include "TGM_Detector.h"

using namespace Tangram;

Detector::Detector()
{

}

Detector::~Detector()
{
    unsigned int numSp = pLibTable->GetNumSpecialRef();
    for (unsigned int i = 0; i != numSp; ++i)
    {
        for (unsigned int j = 0; j != pSpecialEventsTable[i].Size(); ++j)
        {
            if (pSpecialEventsTable[i][j].numFrag[0] > 0)
                free(pSpecialEventsTable[i][j].origIndex[0]);

            if (pSpecialEventsTable[i][j].numFrag[1] > 0)
                free(pSpecialEventsTable[i][j].origIndex[1]);
        }
    }

    if (pSpecialEventsTable != NULL)
        delete [] pSpecialEventsTable;
}

void Detector::Init(const DetectPars* pDetectPars, const LibTable* pLibTable, const BamPairTable* pBamPairTable)
{
    this->pDetectPars = pDetectPars;
    this->pLibTable = pLibTable;
    this->pBamPairTable = pBamPairTable;

    int numSp = pLibTable->GetNumSpecialRef();
    if (numSp > 0)
    {
        pSpecialEventsTable = new (std::nothrow) Array<SpecialEvent>[numSp];
        if (pSpecialEventsTable == NULL)
            TGM_ErrQuit("ERROR: Not enough memory for the special events array.\n");
    }
    else
        pSpecialEventsTable = NULL;

    pairAttrbtTable.Init(pLibTable);
}

void Detector::CallEvents(void)
{
    unsigned int detectSet = pDetectPars->detectSet;

    for (unsigned int i = SV_DELETION; i <= SV_INTER_CHR_TRNSLCTN; ++i)
    {
        switch(i)
        {
            case SV_DELETION:
                if ((detectSet & (1 << i)) != 0)
                    CallDeletion();
                break;
            case SV_TANDEM_DUP:
                if ((detectSet & (1 << i)) != 0)
                    CallTandemDup();
                break;
            case SV_INVERSION:
                if ((detectSet & (1 << i)) != 0)
                    CallInversion();
                break;
            case SV_SPECIAL:
                if ((detectSet & (1 << i)) != 0)
                    CallSpecial();
                break;
            case SV_INTER_CHR_TRNSLCTN:
                if ((detectSet & (1 << i)) != 0)
                    CallTranslocation();
                break;
            default:
                break;
        }
    }
}

void Detector::CallDeletion(void)
{

}

void Detector::CallTandemDup(void)
{

}

void Detector::CallInversion(void)
{

}

void Detector::CallSpecial(void)
{
    pairAttrbtTable.MakeSpecial(pBamPairTable->specialPairs);

    for (unsigned int i = 0; i != pairAttrbtTable.specialSize; i += 2)
    {
        cluster3.Init(pairAttrbtTable.pSpecialAttrbts + i, pDetectPars->minClusterSize, DEFAULT_MIN_STD);
        cluster5.Init(pairAttrbtTable.pSpecialAttrbts + i + 1, pDetectPars->minClusterSize, DEFAULT_MIN_STD);

        cluster3.Make();
        cluster5.Make();

        unsigned int spRefID = i / 2;

        MakeSpecialEvents(pSpecialEventsTable[spRefID]);
        MergeSpecialEvents(pSpecialEventsTable[spRefID]);
        SetOrigIndices(pSpecialEventsTable[spRefID]);
    }
}

void Detector::CallTranslocation(void)
{

}


void Detector::MakeSpecialEvents(Array<SpecialEvent>& specialEvents)
{
    const Array<SpecialPair>& specialPairs = pBamPairTable->specialPairs;

    const Array<ClusterElmnt>* pCluster3 = cluster3.GetCluterElmnts();
    const Array<ClusterElmnt>* pCluster5 = cluster5.GetCluterElmnts();

    const Array<unsigned int>& next3 = cluster3.GetNextArray();
    const Array<unsigned int>& next5 = cluster5.GetNextArray();

    unsigned int actualNum3 = cluster3.GetActualNumElmnts();
    unsigned int actualNum5 = cluster5.GetActualNumElmnts();
    unsigned int actualNum = actualNum3 + actualNum5;

    unsigned int numEvents3 = pCluster3->Size();
    unsigned int numEvents5 = pCluster5->Size();

    specialEvents.Init(actualNum);
    specialEvents.SetSize(actualNum);

    unsigned int spIdx = 0;

    for (unsigned int j = 0; j != numEvents3; ++j)
    {
        const ClusterElmnt* pClusterElmnt = pCluster3->GetPointer(j);

        if (pClusterElmnt->numReadPair == 0)
            continue;

        unsigned int i = pClusterElmnt->startIndex;

        unsigned int posMin5 = UINT_MAX;
        unsigned int endMax5 = 0;

        unsigned int posMin3 = UINT_MAX;
        unsigned int endMax3 = 0;

        SpecialEvent* pSpecialEvent = specialEvents.GetPointer(spIdx);
        pSpecialEvent->fragLenMax = 0;

        ++spIdx;

        const SpecialPair* pSpecialPair = NULL;

        do
        {
            unsigned int origIndex = (*cluster3.pPairAttrbts)[i].origIndex;
            pSpecialPair = specialPairs.GetPointer(origIndex);

            unsigned int fragLenMedian = pLibTable->GetFragLenMedian(pSpecialPair->readGrpID);
            unsigned int fragLenHigh = pLibTable->GetFragLenHigh(pSpecialPair->readGrpID);

            if (fragLenHigh > pSpecialEvent->fragLenMax)
                pSpecialEvent->fragLenMax = fragLenHigh;

            unsigned int posUniq = pSpecialPair->pos[0];
            unsigned int endUniq = pSpecialPair->end[0];

            unsigned int posMultiple = posUniq + fragLenMedian - (pSpecialPair->end[1] - pSpecialPair->pos[1] + 1);
            unsigned int endMultiple = posUniq + fragLenMedian - 1;

            if (posUniq < posMin5)
                posMin5 = posUniq;

            if (endUniq > endMax5)
                endMax5 = endUniq;

            if (posMultiple < posMin3)
                posMin3 = posMultiple;

            if (endMultiple > endMax3)
                endMax3 = endMultiple;

            i = next3[i];

        }while(i != pClusterElmnt->startIndex);

        pSpecialEvent->refID = pSpecialPair->refID[0];

        pSpecialEvent->pos = endMax5;
        pSpecialEvent->numFrag[0] = pClusterElmnt->numReadPair;
        pSpecialEvent->numFrag[1] = 0;
        pSpecialEvent->clusterID[0] = pClusterElmnt->startIndex;

        pSpecialEvent->length = 0;

        pSpecialEvent->pos5[0] = posMin5;
        pSpecialEvent->pos5[1] = endMax5;

        pSpecialEvent->pos3[0] = posMin3;
        pSpecialEvent->pos3[1] = endMax3;

        pSpecialEvent->posUncertainty = DoubleRoundToInt((double) ((endMax5 - posMin5) + (endMax3 - posMin3)) / (double) (2 * pClusterElmnt->numReadPair));
    }

    for (unsigned int j = 0; j != numEvents5; ++j)
    {
        const ClusterElmnt* pClusterElmnt = pCluster5->GetPointer(j);

        if (pClusterElmnt->numReadPair == 0)
            continue;

        unsigned int i = pClusterElmnt->startIndex;

        unsigned int posMin5 = UINT_MAX;
        unsigned int endMax5 = 0;

        unsigned int posMin3 = UINT_MAX;
        unsigned int endMax3 = 0;

        SpecialEvent* pSpecialEvent = specialEvents.GetPointer(spIdx);
        pSpecialEvent->fragLenMax = 0;

        ++spIdx;

        const SpecialPair* pSpecialPair = NULL;

        do
        {
            unsigned int origIndex = (*cluster5.pPairAttrbts)[i].origIndex;
            pSpecialPair = specialPairs.GetPointer(origIndex);

            unsigned int fragLenMedian = pLibTable->GetFragLenMedian(pSpecialPair->readGrpID);
            unsigned int fragLenHigh = pLibTable->GetFragLenHigh(pSpecialPair->readGrpID);

            if (fragLenHigh > pSpecialEvent->fragLenMax)
                pSpecialEvent->fragLenMax = fragLenHigh;

            unsigned int posUniq = pSpecialPair->pos[0];
            unsigned int endUniq = pSpecialPair->end[0];

            unsigned int posMultiple = endUniq + 1 - fragLenMedian;
            unsigned int endMultiple = endUniq + pSpecialPair->end[1] + 1 - fragLenMedian - pSpecialPair->pos[1];

            if (posMultiple < posMin5)
                posMin5 = posMultiple;

            if (endMultiple > endMax5)
                endMax5 = endMultiple;

            if (posUniq < posMin3)
                posMin3 = posUniq;

            if (endUniq > endMax3)
                endMax3 = endUniq;

            i = next5[i];

        }while(i != pClusterElmnt->startIndex);

        pSpecialEvent->refID = pSpecialPair->refID[0];

        pSpecialEvent->pos = posMin3;
        pSpecialEvent->numFrag[0] = 0;
        pSpecialEvent->numFrag[1] = pClusterElmnt->numReadPair;
        pSpecialEvent->clusterID[1] = pClusterElmnt->startIndex;

        pSpecialEvent->length = 0;

        pSpecialEvent->pos5[0] = posMin5;
        pSpecialEvent->pos5[1] = endMax5;

        pSpecialEvent->pos3[0] = posMin3;
        pSpecialEvent->pos3[1] = endMax3;

        pSpecialEvent->posUncertainty = DoubleRoundToInt((double) ((endMax5 - posMin5) + (endMax3 - posMin3)) / (double) (2 * pClusterElmnt->numReadPair));
    }

    specialEvents.Sort(CompareSpecialEvents);
}


void Detector::MergeSpecialEvents(Array<SpecialEvent>& specialEvents)
{
    unsigned int headIndex = 1;
    unsigned int tailIndex = 0;
    unsigned int newSize = specialEvents.Size();
    unsigned int numSpecialEvents = specialEvents.Size();

    while (headIndex < numSpecialEvents)
    {
        SpecialEvent mergedEvent;
        uint32_t fragLenMax = specialEvents[headIndex].fragLenMax;
        if (specialEvents[tailIndex].fragLenMax > fragLenMax)
            fragLenMax = specialEvents[tailIndex].fragLenMax;

        if (abs(specialEvents[headIndex].pos - specialEvents[tailIndex].pos) < fragLenMax)
        {
            SpecialEvent* pHeadEvent = specialEvents.GetPointer(headIndex);
            SpecialEvent* pTailEvent = specialEvents.GetPointer(tailIndex);

            DoSpecialMerge(&mergedEvent, pHeadEvent, pTailEvent);

            *pTailEvent = mergedEvent;

            --newSize;
        }
        else
        {
            ++tailIndex;
            if (headIndex != tailIndex)
            {
                SpecialEvent* pHeadEvent = specialEvents.GetPointer(headIndex);
                SpecialEvent* pTailEvent = specialEvents.GetPointer(tailIndex);
                *pTailEvent = *pHeadEvent;
            }
        }

        ++headIndex;
    }

    specialEvents.SetSize(newSize);
    specialEvents.Sort(CompareSpecialEvents);
}

void Detector::DoSpecialMerge(SpecialEvent* pMergedEvent, const SpecialEvent* pHeadEvent, const SpecialEvent* pTailEvent)
{
    if (pTailEvent->numFrag[0] == 0)
        TGM_SWAP(pHeadEvent, pTailEvent, const SpecialEvent*);

    *pMergedEvent = *pTailEvent;

    bool bothForward = (pHeadEvent->numFrag[0] > 0) && (pTailEvent->numFrag[0] > 0);
    bool bothReversed = (pHeadEvent->numFrag[1] > 0) && (pTailEvent->numFrag[1] > 0);

    if (bothForward)
    {
        pMergedEvent->pos5[0] = (pTailEvent->pos5[0] < pHeadEvent->pos5[0] ? pTailEvent->pos5[0] : pHeadEvent->pos5[0]);
        pMergedEvent->pos5[1] = (pTailEvent->pos5[1] > pHeadEvent->pos5[1] ? pTailEvent->pos5[1] : pHeadEvent->pos5[1]);
        cluster3.Connect(pHeadEvent->clusterID[0], pTailEvent->clusterID[0]);
        pMergedEvent->clusterID[0] = pHeadEvent->clusterID[0];
    }
    else if (bothReversed)
    {
        pMergedEvent->pos3[0] = (pTailEvent->pos3[0] < pHeadEvent->pos3[0] ? pTailEvent->pos3[0] : pHeadEvent->pos3[0]);
        pMergedEvent->pos3[1] = (pTailEvent->pos3[1] > pHeadEvent->pos3[1] ? pTailEvent->pos3[1] : pHeadEvent->pos3[1]);
        cluster5.Connect(pHeadEvent->clusterID[1], pTailEvent->clusterID[1]);
        pMergedEvent->clusterID[1] = pHeadEvent->clusterID[1];
    }
    else
    {
        pMergedEvent->pos3[0] = pHeadEvent->pos3[0];
        pMergedEvent->pos3[1] = pHeadEvent->pos3[1];
        pMergedEvent->clusterID[1] = pHeadEvent->clusterID[1];
    }

    int numFrag5 = pMergedEvent->numFrag[0] = pHeadEvent->numFrag[0] + pTailEvent->numFrag[0];
    pMergedEvent->numFrag[1] = pHeadEvent->numFrag[1] + pTailEvent->numFrag[1];

    int32_t posTail = (pTailEvent->numFrag[0] > 0 ? pTailEvent->pos5[1] : pTailEvent->pos3[0]);
    int32_t posHead = (pHeadEvent->numFrag[1] > 0 ? pHeadEvent->pos3[0] : pHeadEvent->pos5[1]);

    pMergedEvent->fragLenMax = (pHeadEvent->fragLenMax > pTailEvent->fragLenMax ? pHeadEvent->fragLenMax : pTailEvent->fragLenMax);

    pMergedEvent->pos = (posHead < posTail ? posHead : posTail);
    pMergedEvent->posUncertainty = DoubleRoundToInt((double) (pMergedEvent->pos5[1] - pMergedEvent->pos5[0]) / numFrag5);
}

void Detector::SetOrigIndices(Array<SpecialEvent>& specialEvents)
{
    SpecialEvent* pSpecialEvent = NULL;

    unsigned int size = specialEvents.Size();

    for (unsigned int i = 0; i != size; ++i)
    {
        pSpecialEvent = specialEvents.GetPointer(i);
        pSpecialEvent->sense = 0.0;
        pSpecialEvent->splitIdx = -1;

        if (pSpecialEvent->numFrag[0] > 0)
        {
            pSpecialEvent->origIndex[0] = (uint32_t*) malloc(pSpecialEvent->numFrag[0] * sizeof(uint32_t));
            if (pSpecialEvent->origIndex[0] == NULL)
                TGM_ErrQuit("ERROR: Not enough memory for the original index array.\n");

            int centerIndex = pSpecialEvent->clusterID[0];
            int memberIndex = centerIndex;

            unsigned int j = 0;

            do
            {
                unsigned int origIndex = (*cluster3.pPairAttrbts)[memberIndex].origIndex;
                pSpecialEvent->origIndex[0][j] = origIndex;

                const SpecialPair& specialPair = pBamPairTable->specialPairs[origIndex];
                if (specialPair.sStrand == 1)
                    pSpecialEvent->sense += 1.0;
                else
                    pSpecialEvent->sense -= 1.0;

                memberIndex = cluster3.next[memberIndex];
                ++j;

            }while(memberIndex != centerIndex);
        }
        else
            pSpecialEvent->origIndex[0] = NULL;

        if (pSpecialEvent->numFrag[1] > 0)
        {
            pSpecialEvent->origIndex[1] = (uint32_t*) malloc(pSpecialEvent->numFrag[1] * sizeof(uint32_t));
            if (pSpecialEvent->origIndex[1] == NULL)
                TGM_ErrQuit("ERROR: Not enough memory for the original index array.\n");

            int centerIndex = pSpecialEvent->clusterID[1];
            int memberIndex = centerIndex;

            unsigned int j = 0;

            do
            {
                unsigned int origIndex = (*cluster5.pPairAttrbts)[memberIndex].origIndex;
                pSpecialEvent->origIndex[1][j] = origIndex;

                const SpecialPair& specialPair = pBamPairTable->specialPairs[origIndex];
                if (specialPair.sStrand == 0)
                    pSpecialEvent->sense += 1.0;
                else
                    pSpecialEvent->sense -= 1.0;

                memberIndex = cluster5.next[memberIndex];
                ++j;

            }while(memberIndex != centerIndex);
        }
        else
            pSpecialEvent->origIndex[1] = NULL;
    }
}
