/*==============================================================================

  Program: 3D Slicer

  Portions (c) Copyright Brigham and Women's Hospital (BWH) All Rights Reserved.

  See COPYRIGHT.txt
  or http://www.slicer.org/copyright/copyright.txt for details.

  Unless required by applicable law or agreed to in writing, software
  distributed under the License is distributed on an "AS IS" BASIS,
  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
  See the License for the specific language governing permissions and
  limitations under the License.
  
==============================================================================*/  

// PivotCalibration Logic includes
#include "vtkSlicerPivotCalibrationLogic.h"

// MRML includes
#include <vtkMRMLLinearTransformNode.h>
#include "vtkMRMLScene.h"

// VTK includes
#include <vtkNew.h>
#include <vtkSmartPointer.h>
#include <vtkCommand.h>
#include <vtkMatrix4x4.h>
#include <vtkObjectFactory.h>

// STD includes
#include <cassert>

// VNL includes
#include "vnl/algo/vnl_symmetric_eigensystem.h"
#include "vnl/vnl_vector.h"
#include "vnl/algo/vnl_svd.h"


//----------------------------------------------------------------------------
vtkStandardNewMacro(vtkSlicerPivotCalibrationLogic);

//----------------------------------------------------------------------------
vtkSlicerPivotCalibrationLogic::vtkSlicerPivotCalibrationLogic()
{
}

//----------------------------------------------------------------------------
vtkSlicerPivotCalibrationLogic::~vtkSlicerPivotCalibrationLogic()
{
  this->ClearSamples();
}

//----------------------------------------------------------------------------
void vtkSlicerPivotCalibrationLogic::PrintSelf(ostream& os, vtkIndent indent)
{
  this->Superclass::PrintSelf( os, indent );
}

//---------------------------------------------------------------------------
void vtkSlicerPivotCalibrationLogic::SetMRMLSceneInternal(vtkMRMLScene * newScene)
{
  vtkNew<vtkIntArray> events;
  events->InsertNextValue(vtkMRMLScene::NodeAddedEvent);
  events->InsertNextValue(vtkMRMLScene::NodeRemovedEvent);
  events->InsertNextValue(vtkMRMLScene::EndImportEvent);
  events->InsertNextValue(vtkMRMLScene::EndBatchProcessEvent);
  events->InsertNextValue(vtkMRMLScene::StartCloseEvent);
  this->SetAndObserveMRMLSceneEventsInternal(newScene, events.GetPointer());
}

//---------------------------------------------------------------------------
void vtkSlicerPivotCalibrationLogic::ProcessMRMLNodesEvents(vtkObject* caller, unsigned long event, void* callData)
{
  if (caller != NULL)
  {
    vtkMRMLLinearTransformNode* transformNode = vtkMRMLLinearTransformNode::SafeDownCast(caller);
    if (this->recordingState == true && transformNode->GetID() == this->transformId)
    {
      vtkMatrix4x4* matrixToParent = transformNode->GetMatrixTransformToParent();
      vtkMatrix4x4* matrixCopy = vtkMatrix4x4::New();
      matrixCopy->DeepCopy(matrixToParent);
      
      this->AddSample(matrixCopy);
    }
  }
}

//---------------------------------------------------------------------------
void vtkSlicerPivotCalibrationLogic::InitializeObserver(vtkMRMLNode* node)
{
  if (node != NULL)
  {
    vtkMRMLLinearTransformNode* transformNode = vtkMRMLLinearTransformNode::SafeDownCast(node);
    
    this->transformId = transformNode->GetID();
    
    node->AddObserver(vtkMRMLLinearTransformNode::TransformModifiedEvent, (vtkCommand*) this->GetMRMLNodesCallbackCommand());
  }
}

//---------------------------------------------------------------------------
void vtkSlicerPivotCalibrationLogic::AddSample(vtkMatrix4x4* transformMatrix)
{
  this->transforms.push_back(transformMatrix);
}

//---------------------------------------------------------------------------
void vtkSlicerPivotCalibrationLogic::ClearSamples()
{
  std::vector<vtkMatrix4x4*>::const_iterator it, transformsEnd = this->transforms.end();
  for(it = this->transforms.begin(); it != transformsEnd; it++)
  {
    (*it)->Delete();
  }
  this->transforms.clear();
}

//---------------------------------------------------------------------------
void vtkSlicerPivotCalibrationLogic::ComputePivotCalibration()
{
  if (this->transforms.size() > 0)
  {
    unsigned int rows = 3*this->transforms.size();
    unsigned int columns = 6;

    vnl_matrix<double> A(rows, columns), minusI(3,3,0), R(3,3);
    vnl_vector<double> b(rows), x(columns), t(3);
    minusI(0, 0) = -1;
    minusI(1, 1) = -1;
    minusI(2, 2) = -1;
    
    
    std::vector<vtkMatrix4x4*>::const_iterator it, transformsEnd = this->transforms.end();
    unsigned int currentRow;
    for(currentRow = 0, it = this->transforms.begin(); it != transformsEnd; it++, currentRow += 3)
    {
      for (int i = 0; i < 3; i++)
      {
        t(i) = (*it)->GetElement(i, 3);
      }
      t *= -1;
      b.update(t, currentRow);

      for (int i = 0; i < 3; i++)
      {
        for (int j = 0; j < 3; j++ )
        {
          R(i, j) = (*it)->GetElement(i, j);
        }
      }
      A.update(R, currentRow, 0);
      A.update( minusI, currentRow, 3 );    
    }
    
    
    vnl_svd<double> svdA(A);
    
    svdA.zero_out_absolute( 1e-1 );
    
    x = svdA.solve( b );
    
    //set the RMSE
    this->RMSE = ( A * x - b ).rms();

    //set the transformation
    this->Translation[0] = x[0];
    this->Translation[1] = x[1];
    this->Translation[2] = x[2];

    //set the pivot point
    this->PivotPosition[0] = x[3];
    this->PivotPosition[1] = x[4];
    this->PivotPosition[2] = x[5];
  }
}


//---------------------------------------------------------------------------
void vtkSlicerPivotCalibrationLogic::ComputeSpinCalibration()
{
  if ( this->transforms.size() == 0 )
  {
    return;
  }

  // Note: this->transforms is the StylusToReference transforms
  std::vector<vtkMatrix4x4*> StylusToReferenceTransforms = this->transforms;

  // Find average stylus tip point
  vnl_vector<double> meanStylusTipPoint_Reference( 3, 0.0 );

  // This requires a working pivot calibration
  vtkSmartPointer< vtkMatrix4x4 > StylusTipToStylusTransform = vtkSmartPointer< vtkMatrix4x4 >::New();
  StylusTipToStylusTransform->SetElement( 0, 3, this->Translation[ 0 ] );
  StylusTipToStylusTransform->SetElement( 1, 3, this->Translation[ 1 ] );
  StylusTipToStylusTransform->SetElement( 2, 3, this->Translation[ 2 ] );

  // Find the plane of best fit for the points
  vnl_matrix<double> stylusPoints_Reference( this->transforms.size(), 3, 0.0 );
  vnl_vector<double> meanStylusPoint_Reference( 3, 0.0 );

  for ( int i = 0; i < StylusToReferenceTransforms.size(); i++ )
  {
    stylusPoints_Reference.put( i, 0, StylusToReferenceTransforms.at( i )->GetElement( 0, 3 ) );
    stylusPoints_Reference.put( i, 1, StylusToReferenceTransforms.at( i )->GetElement( 1, 3 ) );
    stylusPoints_Reference.put( i, 2, StylusToReferenceTransforms.at( i )->GetElement( 2, 3 ) );

    meanStylusPoint_Reference.put( 0, meanStylusPoint_Reference.get( 0 ) + StylusToReferenceTransforms.at( i )->GetElement( 0, 3 ) );
    meanStylusPoint_Reference.put( 1, meanStylusPoint_Reference.get( 1 ) + StylusToReferenceTransforms.at( i )->GetElement( 1, 3 ) );
    meanStylusPoint_Reference.put( 2, meanStylusPoint_Reference.get( 2 ) + StylusToReferenceTransforms.at( i )->GetElement( 2, 3 ) );
  }

  meanStylusPoint_Reference.put( 0, meanStylusPoint_Reference.get( 0 ) / StylusToReferenceTransforms.size() );
  meanStylusPoint_Reference.put( 1, meanStylusPoint_Reference.get( 1 ) / StylusToReferenceTransforms.size() );
  meanStylusPoint_Reference.put( 2, meanStylusPoint_Reference.get( 2 ) / StylusToReferenceTransforms.size() );

  vnl_matrix<double> covariance = stylusPoints_Reference.transpose() * stylusPoints_Reference / StylusToReferenceTransforms.size() - outer_product( meanStylusPoint_Reference, meanStylusPoint_Reference );
  
  vnl_matrix<double> eigenvectors( 3, 3, 0.0 );
  vnl_vector<double> eigenvalues( 3, 0.0 );
  vnl_symmetric_eigensystem_compute( covariance, eigenvectors, eigenvalues );
  // Note: eigenvectors are ordered in increasing eigenvalue ( 0 = smallest, end = biggest )

  vnl_vector<double> vector1 = eigenvectors.get_column( 1 );
  vnl_vector<double> vector2 = eigenvectors.get_column( 2 );

  // Project positions onto plane
  vnl_matrix<double> FitCircleMatrix( stylusPoints_Reference.rows(), 3, 0.0 );
  vnl_vector<double> FitCircleVector( stylusPoints_Reference.rows(), 0.0 );

  for ( int i = 0; i < stylusPoints_Reference.rows(); i++ )
  {
    vnl_vector<double> currentStylusPoint_Reference = stylusPoints_Reference.get_row( i ) - meanStylusPoint_Reference;
    vnl_vector<double> currentStylusTip_Projection( 2, 0.0 );
    currentStylusTip_Projection.put( 0, dot_product( currentStylusPoint_Reference, vector1 ) );
    currentStylusTip_Projection.put( 1, dot_product( currentStylusPoint_Reference, vector2 ) );

    FitCircleMatrix.put( i, 0, 2 * currentStylusTip_Projection.get( 0 ) );
    FitCircleMatrix.put( i, 1, 2 * currentStylusTip_Projection.get( 1 ) );
    FitCircleMatrix.put( i, 2, 1 );

    FitCircleVector.put( i, currentStylusTip_Projection.get( 0 ) * currentStylusTip_Projection.get( 0 ) + currentStylusTip_Projection.get( 1 ) * currentStylusTip_Projection.get( 1 ) );
  }

  // Solve the system
  vnl_svd<double> CircleSolver( FitCircleMatrix );
  vnl_vector<double> stylusRotationCentre_Projection = CircleSolver.solve( FitCircleVector );

  // Unproject the centre
  vnl_vector<double> stylusRotationCentre_Reference = stylusRotationCentre_Projection.get( 0 ) * vector1 + stylusRotationCentre_Projection.get( 1 ) * vector2 + meanStylusPoint_Reference;
  
  // Put into the StylusTip coordinate system
  double arrayStylusRotationCentre_Reference[ 4 ] = { stylusRotationCentre_Reference[ 0 ], stylusRotationCentre_Reference[ 1 ], stylusRotationCentre_Reference[ 2 ], 1 };
  double arrayStylusRotationCentre_StylusTip[ 4 ] = { 0, 0, 0, 1 };

  vtkSmartPointer< vtkMatrix4x4 > ReferenceToStylusTipTransform = vtkSmartPointer< vtkMatrix4x4 >::New();
  vtkSmartPointer< vtkMatrix4x4 > ReferenceToStylusTransform = vtkSmartPointer< vtkMatrix4x4 >::New();
  vtkSmartPointer< vtkMatrix4x4 > StylusToStylusTipTransform = vtkSmartPointer< vtkMatrix4x4 >::New();
  StylusToStylusTipTransform->DeepCopy( StylusTipToStylusTransform );
  StylusToStylusTipTransform->Invert();

  for ( int i = 0; i < this->transforms.size(); i++ )
  {
    ReferenceToStylusTransform->DeepCopy( StylusToReferenceTransforms.at( i ) );
    ReferenceToStylusTransform->Invert();
    vtkMatrix4x4::Multiply4x4( StylusToStylusTipTransform, ReferenceToStylusTransform, ReferenceToStylusTipTransform );
    
    double currentArrayStylusRotationCentre_StylusTip[ 4 ] = { 0, 0, 0, 1 };
    ReferenceToStylusTipTransform->MultiplyPoint( arrayStylusRotationCentre_Reference, currentArrayStylusRotationCentre_StylusTip );

    arrayStylusRotationCentre_StylusTip[ 0 ] += currentArrayStylusRotationCentre_StylusTip[ 0 ];
    arrayStylusRotationCentre_StylusTip[ 1 ] += currentArrayStylusRotationCentre_StylusTip[ 1 ];
    arrayStylusRotationCentre_StylusTip[ 2 ] += currentArrayStylusRotationCentre_StylusTip[ 2 ];
    arrayStylusRotationCentre_StylusTip[ 3 ] += currentArrayStylusRotationCentre_StylusTip[ 3 ];
  }

  arrayStylusRotationCentre_StylusTip[ 0 ] /= StylusToReferenceTransforms.size();
  arrayStylusRotationCentre_StylusTip[ 1 ] /= StylusToReferenceTransforms.size();
  arrayStylusRotationCentre_StylusTip[ 2 ] /= StylusToReferenceTransforms.size();
  arrayStylusRotationCentre_StylusTip[ 3 ] /= StylusToReferenceTransforms.size();

  // This is the shaft point in the StylusTip coordinate frame
  vnl_vector<double> stylusRotationCentre_StylusTip( 3, 0.0 );
  stylusRotationCentre_StylusTip.put( 0, arrayStylusRotationCentre_StylusTip[ 0 ] );
  stylusRotationCentre_StylusTip.put( 1, arrayStylusRotationCentre_StylusTip[ 1 ] );
  stylusRotationCentre_StylusTip.put( 2, arrayStylusRotationCentre_StylusTip[ 2 ] );

  vnl_vector<double> yPoint_StylusTip( 3, 0.0 );
  yPoint_StylusTip.put( 1, 1 ); // Put the y part in
  yPoint_StylusTip = yPoint_StylusTip - dot_product( yPoint_StylusTip, stylusRotationCentre_StylusTip ) * stylusRotationCentre_StylusTip;
  yPoint_StylusTip.normalize();

  // Register X,Y,O points in the two coordinate frames (only spherical registration - since pure rotation)
  vnl_matrix<double> StylusTipPoints( 3, 3, 0.0 );
  vnl_matrix<double> XShaftPoints( 3, 3, 0.0 );

  StylusTipPoints.put( 0, 0, stylusRotationCentre_StylusTip.get( 0 ) );
  StylusTipPoints.put( 0, 1, stylusRotationCentre_StylusTip.get( 1 ) );
  StylusTipPoints.put( 0, 2, stylusRotationCentre_StylusTip.get( 2 ) );
  StylusTipPoints.put( 1, 0, yPoint_StylusTip.get( 0 ) );
  StylusTipPoints.put( 1, 1, yPoint_StylusTip.get( 1 ) );
  StylusTipPoints.put( 1, 2, yPoint_StylusTip.get( 2 ) );
  StylusTipPoints.put( 2, 0, 0 );
  StylusTipPoints.put( 2, 1, 0 );
  StylusTipPoints.put( 2, 2, 0 );

  XShaftPoints.put( 0, 0, 1 );
  XShaftPoints.put( 0, 1, 0 );
  XShaftPoints.put( 0, 2, 0 );
  XShaftPoints.put( 1, 0, 0 );
  XShaftPoints.put( 1, 1, 1 );
  XShaftPoints.put( 1, 2, 0 );
  XShaftPoints.put( 2, 0, 0 );
  XShaftPoints.put( 2, 1, 0 );
  XShaftPoints.put( 2, 2, 0 );
  
  vnl_svd<double> registrator( XShaftPoints.transpose() * StylusTipPoints );
    
  this->Rotation = registrator.V() * registrator.U().transpose();
}


//-----------------------------------------------------------------------------
void vtkSlicerPivotCalibrationLogic::setRecordingState(bool state)
{
  this->recordingState = state;
}


/*
//---------------------------------------------------------------------------
void vtkSlicerPivotCalibrationLogic
::OnMRMLSceneNodeAdded(vtkMRMLNode* vtkNotUsed(node))
{
}

//---------------------------------------------------------------------------
void vtkSlicerPivotCalibrationLogic
::OnMRMLSceneNodeRemoved(vtkMRMLNode* vtkNotUsed(node))
{
}
//*/

