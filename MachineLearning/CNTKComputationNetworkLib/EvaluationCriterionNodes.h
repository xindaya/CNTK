//
// <copyright file="EvaluationCriterionNodes.h" company="Microsoft">
//     Copyright (c) Microsoft Corporation.  All rights reserved.
// </copyright>
//
#pragma once

#include <map>
#include <string>
#include <vector>
#include <stdexcept>
#include <list>
#include <memory>
#include "ComputationNode.h"

namespace Microsoft { namespace MSR { namespace CNTK {
    //note: to save computation the gradient may be scaled by an constant. 

    // -----------------------------------------------------------------------
    // ErrorPredictionNode (label, prediction)    --TODO: is that correct?
    // -----------------------------------------------------------------------

    template<class ElemType>
    class ErrorPredictionNode : public ComputationNodeNonLooping/*ComputationNode*/<ElemType>, public NumInputs<2>
    {
        typedef ComputationNodeNonLooping<ElemType> Base; UsingComputationNodeMembersBoilerplate;
        static const std::wstring TypeName() { return L"ErrorPrediction"; }
    public:
        ErrorPredictionNode(DEVICEID_TYPE deviceId, const wstring & name) :
            Base(deviceId, name),
            m_maxIndexes0(deviceId), m_maxIndexes1(deviceId), m_maxValues(deviceId)
        { }

        void Reset()
        {
        }

        virtual void ComputeInputPartial(const size_t /*inputIndex*/)  //scaled by 2*number of elements in the Matrix<ElemType>
        {
            LogicError("ErrorPrediction is used for evaluation only.");
        }

        virtual void /*ComputationNodeNonLooping::*/EvaluateThisNodeNonLooping() override
        {
            EvaluateThisNodeS(m_functionValues, Inputs(0)->FunctionValues(), Inputs(1)->FunctionValues(), m_maxIndexes0, m_maxIndexes1, m_maxValues, shared_from_this());
        }

        void EvaluateThisNodeS(Matrix<ElemType>& functionValues, const Matrix<ElemType>& inputFunctionValues0, const Matrix<ElemType>& inputFunctionValues1, Matrix<ElemType>& maxIndexes0, Matrix<ElemType>& maxIndexes1, Matrix<ElemType>& maxValues, ComputationNodePtr curNode)
        {
            inputFunctionValues0.VectorMax(maxIndexes0, maxValues, true);
            inputFunctionValues1.VectorMax(maxIndexes1, maxValues, true);
            curNode->MaskMissingColumnsToZero(maxIndexes0); //we are fine since it will only be called with full minibatch
            curNode->MaskMissingColumnsToZero(maxIndexes1);
            functionValues.AssignNumOfDiff(maxIndexes0, maxIndexes1);
        #if NANCHECK
            functionValues.HasNan("ErrorPrediction");
        #endif
#if DUMPOUTPUT
            functionValues.Print("ErrorPredictionNode");
#endif
        }

        virtual void /*ComputationNodeBase::*/Validate() override
        {
            Base::Validate();

            size_t index = 0;
            // TODO: use dynamic_pointer_cast instead
            if (Inputs(index)->OperationName() == OperationNameOf(LearnableParameter))
            {
                size_t rows = Inputs(index)->FunctionValues().GetNumRows() == 0? Inputs(1-index)->FunctionValues().GetNumRows() : Inputs(index)->FunctionValues().GetNumRows();
                size_t cols = Inputs(index)->FunctionValues().GetNumCols() == 0? Inputs(1-index)->FunctionValues().GetNumCols() : Inputs(index)->FunctionValues().GetNumCols();
                Inputs(index)->FunctionValues().Resize(rows, cols);
            }

            index = 1;
            if (Inputs(index)->OperationName() == OperationNameOf(LearnableParameter))
            {
                size_t rows = Inputs(index)->FunctionValues().GetNumRows() == 0? Inputs(1-index)->FunctionValues().GetNumRows() : Inputs(index)->FunctionValues().GetNumRows();
                size_t cols = Inputs(index)->FunctionValues().GetNumCols() == 0? Inputs(1-index)->FunctionValues().GetNumCols() : Inputs(index)->FunctionValues().GetNumCols();
                Inputs(index)->FunctionValues().Resize(rows, cols);
                m_maxIndexes0.Resize(1,cols);
                m_maxIndexes1.Resize(1,cols);
                m_maxValues.Resize(1,cols);
            }

            if (Inputs(0)->FunctionValues().HasNoElements() || Inputs(1)->FunctionValues().HasNoElements())
                LogicError("ErrorPrediction operation: one of the operants has 0 element.");

            if (((!(Inputs(0)->FunctionValues().GetNumRows() == Inputs(1)->FunctionValues().GetNumRows()  &&  //match size
                Inputs(0)->FunctionValues().GetNumCols() == Inputs(1)->FunctionValues().GetNumCols()) )) && Inputs(0)->GetLoopId() < 0)
            {
                LogicError("The Matrix dimension in the ErrorPrediction operation does not match.");
            }       

            FunctionValues().Resize(1,1);
            m_pMBLayout = nullptr;    // this node does not hold mini-batch data
            InferImageDimsFromInputs(); 

            // resize the temporaries to their proper size
            size_t cols = Inputs(0)->FunctionValues().GetNumCols();
            m_maxIndexes0.Resize(1,cols);
            m_maxIndexes1.Resize(1,cols);
            m_maxValues.Resize(1,cols);
        }

        virtual void InferImageDimsFromInputs()
        {
            InferImageDimsFromInput(0, false);

            m_outputChannels = 1;
            m_outputWidth = 1;
            m_outputHeight = 1;        
        }

        //virtual void AttachInputs(const ComputationNodePtr leftNode, const ComputationNodePtr rightNode) 
        //{
        //    m_children.resize(2);
        //    m_children[0] = leftNode;
        //    m_children[1] = rightNode;
        //}

        virtual void MoveMatricesToDevice(const DEVICEID_TYPE deviceId)
        {
            Base::MoveMatricesToDevice(deviceId);
            m_maxIndexes0.TransferToDeviceIfNotThereAndNotAutoPlace(deviceId, true);
            m_maxIndexes1.TransferToDeviceIfNotThereAndNotAutoPlace(deviceId, true);
            m_maxValues.TransferToDeviceIfNotThereAndNotAutoPlace(deviceId, true);
        }

        virtual void CopyTo(const ComputationNodePtr nodeP, const std::wstring& newName, const CopyNodeFlags flags) const
        {
            Base::CopyTo(nodeP, newName, flags);
            if (flags & CopyNodeFlags::copyNodeValue)
            {
                auto node = dynamic_pointer_cast<ErrorPredictionNode<ElemType>>(nodeP);
                node->m_maxIndexes0 = m_maxIndexes0;
                node->m_maxIndexes1 = m_maxIndexes1;
                node->m_maxValues = m_maxValues;
            }
        }
protected:
        virtual bool NodeDoesItsOwnCustomizedMissingColumnsMasking() { return true; }

    private:
        Matrix<ElemType> m_maxIndexes0, m_maxIndexes1;
        Matrix<ElemType> m_maxValues;
    };

    template class ErrorPredictionNode<float>; 
    template class ErrorPredictionNode<double>;

}}}