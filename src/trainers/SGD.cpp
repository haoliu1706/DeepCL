// Copyright Hugh Perkins 2015 hughperkins at gmail
//
// This Source Code Form is subject to the terms of the Mozilla Public License, 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.

#include <iostream>

#include "util/stringhelper.h"
#include "net/NeuralNet.h"
#include "layer/Layer.h"
#include "loss/LossLayer.h"
#include "trainers/SGDStateMaker.h"
#include "trainers/SGDState.h"
#include "trainers/SGD.h"
#include "loss/IAcceptsLabels.h"
#include "batch/NetAction.h"
#include "clmath/MultiplyInPlace.h"
#include "clmath/CopyBuffer.h"
#include "clmath/GpuAdd.h"

using namespace std;

#undef STATIC
#undef VIRTUAL
#define STATIC
#define VIRTUAL


VIRTUAL SGD::~SGD() {
//    delete kernel;
    delete multiplyInPlace;
    delete copyBuffer;
    delete gpuAdd;
}
VIRTUAL void SGD::setMomentum( float momentum ) {
    this->momentum = momentum;
}
VIRTUAL void SGD::setWeightDecay( float weightDecay ) {
    this->weightDecay = weightDecay;
}
VIRTUAL std::string SGD::asString() {
    return "SGD{ learningRate=" + toString( learningRate ) + ", momentum=" + 
        toString( momentum ) + " }";
}
VIRTUAL void SGD::updateWeights( CLWrapper *weightsWrapper, CLWrapper *gradWeightsWrapper,
        SGDState *trainerState ) {
    int numWeights = trainerState->numWeights;
    CLWrapper *lastUpdateWrapper = trainerState->lastUpdateWrapper;
    float *gradWeightsCopy = new float[ numWeights ];
    CLWrapper *gradWeightsCopyWrapper = cl->wrap( numWeights, gradWeightsCopy );
    gradWeightsCopyWrapper->createOnDevice();

    // we should probably either write a wrapper class for doing maths,
    // or else use clBLAS etc
    // dweights <= mom * dweights
    multiplyInPlace->multiply( numWeights, momentum, lastUpdateWrapper );

    // gradweightscopy <= - learningRate * gradWeights
    copyBuffer->copy( numWeights, gradWeightsWrapper, gradWeightsCopyWrapper );
    multiplyInPlace->multiply( numWeights, - learningRate, gradWeightsCopyWrapper );

    // dweights <= mom * dweights - learningRate * gradWeights
    gpuAdd->add( numWeights, lastUpdateWrapper, gradWeightsCopyWrapper );

    // weights <= weights + dweights
    gpuAdd->add( numWeights, weightsWrapper, lastUpdateWrapper );

    delete gradWeightsCopyWrapper;
    delete[] gradWeightsCopy;

//    CLWrapper *lastUpdateWrapper = trainerState->lastUpdateWrapper;
//    kernel  ->in( numWeights )
//            ->in( learningRate )
//            ->in( momentum )
//            ->inout( lastUpdateWrapper )
//            ->in( gradWeightsWrapper )
//            ->inout( weightsWrapper );
//    int globalSize = numWeights;
//    int workgroupSize = 64;
//    int numWorkgroups = ( globalSize + workgroupSize - 1 ) / workgroupSize;
//    kernel->run_1d( numWorkgroups * workgroupSize, workgroupSize );
//    cl->finish();

    if( weightDecay > 0 ) {
        // apply weight decay, by multiplying the weights by (1.0f - weightDecay)
        // so weightDecay == 0 means no decay; and weightDecay == 1.0f means
        // weights go immediately to zero
        multiplyInPlace->multiply( numWeights, 1.0f - weightDecay, weightsWrapper );
    }
}
VIRTUAL BatchResult SGD::train( NeuralNet *net, TrainingContext *context,
    float const*input, float const*expectedOutput ) {
    // learns one batch, including updating weights
    // doesnt have to think about running multiple batches,
    // or loading data, or anything like that
    bindState( net );

    net->forward( input );
    float loss = net->calcLoss( expectedOutput );
    net->backward( expectedOutput );

    int numLayers = net->getNumLayers();
    for( int layerIdx = numLayers - 2; layerIdx > 0; layerIdx-- ) {
        Layer *layer = net->getLayer( layerIdx );
        if( !layer->needsBackProp() ) {
            break;
        }
        if( layer->needsTrainerState() ) {
            updateWeights( layer->getWeightsWrapper(), layer->getGradWeightsWrapper(), 
                dynamic_cast< SGDState * >( layer->getTrainerState() ) );
            if( layer->biased() ) {
                updateWeights( layer->getBiasWrapper(), layer->getGradBiasWrapper(),
                    dynamic_cast< SGDState * >( layer->getBiasTrainerState() ) );
            }
        }
    }
    return BatchResult( loss, 0 );
}
VIRTUAL BatchResult SGD::trainFromLabels( NeuralNet *net, TrainingContext *context,
    float const*input, int const*labels ) {
    // learns one batch, including updating weights
    // doesnt have to think about running multiple batches,
    // or loading data, or anything like that

    bindState( net );

    net->forward( input );
    int numRight = net->calcNumRight( labels );
    float loss = net->calcLossFromLabels( labels );
    net->backwardFromLabels( labels );

    int numLayers = net->getNumLayers();
    for( int layerIdx = numLayers - 2; layerIdx > 0; layerIdx-- ) {
        Layer *layer = net->getLayer( layerIdx );
        if( !layer->needsBackProp() ) {
            break;
        }
        if( layer->needsTrainerState() ) {
            updateWeights( layer->getWeightsWrapper(), layer->getGradWeightsWrapper(), 
                dynamic_cast< SGDState * >( layer->getTrainerState() ) );
            if( layer->biased() ) {
                updateWeights( layer->getBiasWrapper(), layer->getGradBiasWrapper(),
                    dynamic_cast< SGDState * >( layer->getBiasTrainerState() ) );
            }
        }
    }
    return BatchResult( loss, numRight );
}
VIRTUAL void SGD::bindState( NeuralNet *net ) {
    SGDStateMaker stateMaker;
    // go through network layers, and assign SGD objects (should probably rename these sometime somehow)
    for( int layerIdx = 0; layerIdx < net->getNumLayers(); layerIdx++ ) {
        Layer *layer = net->getLayer( layerIdx );
        if( layer->needsTrainerState() ) {
            TrainerState *state = layer->getTrainerState();
            SGDState *sgdState = dynamic_cast< SGDState *>( state );
            if( sgdState == 0 ) {
                layer->setTrainerState( &stateMaker );
            }
        }
    }
}
STATIC SGD *SGD::instance( EasyCL *cl, float learningRate ) {
    SGD *sgd = new SGD( cl );
    sgd->setLearningRate( learningRate );
    return sgd;
}
STATIC SGD *SGD::instance( EasyCL *cl, float learningRate, float momentum ) {
    SGD *sgd = new SGD( cl );
    sgd->setLearningRate( learningRate );
    sgd->setMomentum( momentum );
    return sgd;
}
SGD::SGD( EasyCL *cl ) :
        Trainer( cl ),
//        kernel( 0 ),
        momentum( 0.0f ),
        weightDecay( 0.0f ) {
    string options = "";
    multiplyInPlace = new MultiplyInPlace( cl );
    gpuAdd = new GpuAdd( cl );
    copyBuffer = new CopyBuffer( cl );

    // [[[cog
    // # import stringify
    // # stringify.write_kernel2( "kernel", "cl/SGD.cl", "updateWeights", 'options' )
    // ]]]
    // [[[end]]]
}
