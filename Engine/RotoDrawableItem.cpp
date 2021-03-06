/* ***** BEGIN LICENSE BLOCK *****
 * This file is part of Natron <http://www.natron.fr/>,
 * Copyright (C) 2013-2017 INRIA and Alexandre Gauthier-Foichat
 *
 * Natron is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * Natron is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Natron.  If not, see <http://www.gnu.org/licenses/gpl-2.0.html>
 * ***** END LICENSE BLOCK ***** */

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "RotoDrawableItem.h"

#include <algorithm> // min, max
#include <sstream>
#include <locale>
#include <limits>
#include <cassert>
#include <cstring> // for std::memcpy
#include <stdexcept>

#include <QtCore/QLineF>
#include <QtCore/QDebug>

GCC_DIAG_UNUSED_LOCAL_TYPEDEFS_OFF
// /usr/local/include/boost/bind/arg.hpp:37:9: warning: unused typedef 'boost_static_assert_typedef_37' [-Wunused-local-typedef]
#include <boost/bind.hpp>
#include <boost/shared_ptr.hpp>
#include <boost/math/special_functions/fpclassify.hpp>
GCC_DIAG_UNUSED_LOCAL_TYPEDEFS_ON

#include "Engine/AppInstance.h"
#include "Engine/BezierCP.h"
#include "Engine/Bezier.h"
#include "Engine/CreateNodeArgs.h"
#include "Engine/CoonsRegularization.h"
#include "Engine/FeatherPoint.h"
#include "Engine/Format.h"
#include "Engine/Hash64.h"
#include "Engine/Image.h"
#include "Engine/Node.h"
#include "Engine/KnobTypes.h"
#include "Engine/MergingEnum.h"
#include "Engine/Interpolation.h"
#include "Engine/Project.h"
#include "Engine/RenderStats.h"
#include "Engine/RotoLayer.h"
#include "Engine/RotoStrokeItem.h"
#include "Engine/RotoPaint.h"
#include "Engine/RotoPaintPrivate.h"
#include "Engine/RotoShapeRenderNode.h"
#include "Engine/Settings.h"
#include "Engine/TimeLine.h"
#include "Engine/Transform.h"
#include "Engine/ViewIdx.h"
#include "Engine/ViewerInstance.h"

#include "Serialization/NodeSerialization.h"

// When defined items are blurred using a TimeBlur node instead of doing it natively in their render action
//#define ROTOPAINT_MOTIONBLUR_USE_TIMEBLUR

NATRON_NAMESPACE_ENTER;


struct RotoDrawableItemPrivate
{
    Q_DECLARE_TR_FUNCTIONS(RotoDrawableItem)

public:


    /*
     * The effect node corresponds to the following given the selected tool:
     * Stroke= RotoOFX
     * Blur = BlurCImg
     * Clone = TransformOFX
     * Sharpen = SharpenCImg
     * Smear = hand made tool
     * Reveal = Merge(over) with A being color type and B the tree upstream
     * Dodge/Burn = Merge(color-dodge/color-burn) with A being the tree upstream and B the color type
     *
     * Each effect is followed by a merge (except for the ones that use a merge) with the user given operator
     * onto the previous tree upstream of the effectNode.
     */
    NodePtr effectNode, maskNode;
    NodePtr mergeNode;
    NodePtr timeOffsetNode, frameHoldNode;
#ifdef ROTOPAINT_MOTIONBLUR_USE_TIMEBLUR
    NodePtr timeBlurNode;
#endif
    std::list<NodePtr> nodes;
    KnobColorWPtr overlayColor; //< the color the shape overlay should be drawn with, defaults to smooth red
    KnobDoubleWPtr opacity; //< opacity of the rendered shape between 0 and 1

    KnobChoiceWPtr lifeTime;
    KnobBoolWPtr customRange;
    KnobIntWPtr lifeTimeFrame;
    KnobButtonWPtr invertKnob; //< invert the rendering
    KnobColorWPtr color;
    KnobChoiceWPtr compOperator;

    KnobDoubleWPtr brushSize;
    KnobDoubleWPtr brushSpacing;
    KnobDoubleWPtr brushHardness;
    KnobDoubleWPtr visiblePortion; // [0,1] by default

    // Transform
    KnobDoubleWPtr translate;
    KnobDoubleWPtr rotate;
    KnobDoubleWPtr scale;
    KnobBoolWPtr scaleUniform;
    KnobDoubleWPtr skewX;
    KnobDoubleWPtr skewY;
    KnobChoiceWPtr skewOrder;
    KnobDoubleWPtr center;
    KnobDoubleWPtr extraMatrix;

    // Motion blur
    KnobIntWPtr motionBlurAmount;
    KnobDoubleWPtr motionBlurShutter;
    KnobChoiceWPtr motionBlurShutterType;
    KnobDoubleWPtr motionBlurCustomShutter;

    // used for reveal/clone/comp nitem to select the input node for the A input of the merge
    KnobChoiceWPtr mergeAInputChoice;

    // used by the comp item only to select the mask for the merge
    KnobChoiceWPtr mergeMaskInputChoice;

    KnobIntWPtr timeOffset;
    KnobChoiceWPtr timeOffsetMode;
    KnobDoubleWPtr mixKnob;

    RotoDrawableItemPrivate()
    {

    }

    RotoDrawableItemPrivate(const RotoDrawableItemPrivate& other)
    : effectNode(other.effectNode)
    , maskNode(other.maskNode)
    , mergeNode(other.mergeNode)
    , timeOffsetNode(other.timeOffsetNode)
    , frameHoldNode(other.frameHoldNode)
#ifdef ROTOPAINT_MOTIONBLUR_USE_TIMEBLUR
    , timeBlurNode(other.timeBlurNode)
#endif
    , nodes(other.nodes)
    , overlayColor(other.overlayColor)
    , opacity(other.opacity)
    , lifeTime(other.lifeTime)
    , customRange(other.customRange)
    , lifeTimeFrame(other.lifeTimeFrame)
    , invertKnob(other.invertKnob)
    , color(other.color)
    , compOperator(other.compOperator)
    , brushSize(other.brushSize)
    , brushSpacing(other.brushSpacing)
    , brushHardness(other.brushHardness)
    , visiblePortion(other.visiblePortion)
    , translate(other.translate)
    , rotate(other.rotate)
    , scale(other.scale)
    , scaleUniform(other.scaleUniform)
    , skewX(other.skewX)
    , skewY(other.skewY)
    , skewOrder(other.skewOrder)
    , center(other.center)
    , extraMatrix(other.extraMatrix)
    , motionBlurAmount(other.motionBlurAmount)
    , motionBlurShutter(other.motionBlurShutter)
    , motionBlurShutterType(other.motionBlurShutterType)
    , motionBlurCustomShutter(other.motionBlurCustomShutter)
    , mergeAInputChoice(other.mergeAInputChoice)
    , mergeMaskInputChoice(other.mergeMaskInputChoice)
    , timeOffset(other.timeOffset)
    , timeOffsetMode(other.timeOffsetMode)
    , mixKnob(other.mixKnob)
    {

    }

};

RotoDrawableItem::RotoDrawableItem(const KnobItemsTablePtr& model)
    : RotoItem(model)
    , _imp( new RotoDrawableItemPrivate() )
{
}


RotoDrawableItem::RotoDrawableItem(const RotoDrawableItemPtr& other, const TreeRenderPtr& render)
: RotoItem(other, render)
, _imp(new RotoDrawableItemPrivate(*other->_imp))
{

}

RotoDrawableItem::~RotoDrawableItem()
{
}


const NodesList&
RotoDrawableItem::getItemNodes() const
{
    return _imp->nodes;
}


void
RotoDrawableItem::setNodesThreadSafetyForRotopainting()
{
    assert( toRotoStrokeItem( boost::dynamic_pointer_cast<RotoDrawableItem>( shared_from_this() ) ) );

    KnobItemsTablePtr model = getModel();
    if (!model) {
        return;
    }
    NodePtr node = model->getNode();
    if (!node) {
        return;
    }
    RotoPaintPtr isRotopaint = toRotoPaint(node->getEffectInstance());
    node->getEffectInstance()->setRenderThreadSafety(eRenderSafetyInstanceSafe);
    for (NodesList::iterator it = _imp->nodes.begin(); it != _imp->nodes.end(); ++it) {
        (*it)->getEffectInstance()->setRenderThreadSafety(eRenderSafetyInstanceSafe);
    }

}

static void attachStrokeToNode(const NodePtr& node, const NodePtr& rotopaintNode, const RotoDrawableItemPtr& item)
{
    assert(rotopaintNode);
    assert(node);
    assert(item);
    node->getEffectInstance()->attachRotoItem(item);

    // Link OpenGL enabled knob to the one on the Rotopaint so the user can control if GPU rendering is used in the roto internal node graph
    KnobChoicePtr glRenderKnob = node->getEffectInstance()->getOrCreateOpenGLEnabledKnob();
    if (glRenderKnob) {
        KnobChoicePtr rotoPaintGLRenderKnob = rotopaintNode->getEffectInstance()->getOrCreateOpenGLEnabledKnob();
        assert(rotoPaintGLRenderKnob);
        ignore_result(glRenderKnob->linkTo(rotoPaintGLRenderKnob));
    }
}

void
RotoDrawableItem::createNodes(bool connectNodes)
{

    KnobItemsTablePtr model = getModel();
    if (!model) {
        return;
    }
    NodePtr node = model->getNode();
    if (!node) {
        return;
    }

    RotoDrawableItemPtr thisShared = boost::dynamic_pointer_cast<RotoDrawableItem>( shared_from_this() );
    RotoPaintPtr rotoPaintEffect = toRotoPaint(node->getEffectInstance());
    assert(rotoPaintEffect);
    rotoPaintEffect->refreshSourceKnobs(thisShared);
    
    AppInstancePtr app = rotoPaintEffect->getApp();

    QString fixedNamePrefix = QString::fromUtf8( rotoPaintEffect->getNode()->getScriptName_mt_safe().c_str() );
    fixedNamePrefix.append( QLatin1Char('_') );
    fixedNamePrefix.append( QString::fromUtf8( getScriptName_mt_safe().c_str() ) );

    QString pluginId;
    RotoStrokeType type = getBrushType();
    assert(thisShared);
    RotoStrokeItemPtr isStroke = toRotoStrokeItem(thisShared);

    const QString maskPluginID = QString::fromUtf8(PLUGINID_NATRON_ROTOSHAPE);

    switch (type) {
    case eRotoStrokeTypeBlur:
        pluginId = QString::fromUtf8(PLUGINID_OFX_BLURCIMG);
        break;
    case eRotoStrokeTypeEraser:
        pluginId = QString::fromUtf8(PLUGINID_OFX_CONSTANT);
        break;
    case eRotoStrokeTypeSolid:
    case eRotoStrokeTypeSmear:
        pluginId = maskPluginID;
        break;
    case eRotoStrokeTypeClone:
    case eRotoStrokeTypeReveal:
        pluginId = QString::fromUtf8(PLUGINID_OFX_TRANSFORM);
        break;
    case eRotoStrokeTypeBurn:
    case eRotoStrokeTypeDodge:
        //uses merge
        break;
    case eRotoStrokeTypeSharpen:
        pluginId = QString::fromUtf8(PLUGINID_OFX_SHARPENCIMG);
        break;
    case eRotoStrokeTypeComp:
        // No node
        break;
    }

    QString baseFixedName = fixedNamePrefix;
    if ( !pluginId.isEmpty() ) {
        fixedNamePrefix.append( QString::fromUtf8("Effect") );

        CreateNodeArgsPtr args(CreateNodeArgs::create( pluginId.toStdString(), rotoPaintEffect ));
        args->setProperty<bool>(kCreateNodeArgsPropVolatile, true);
#ifndef ROTO_PAINT_NODE_GRAPH_VISIBLE
        args->setProperty<bool>(kCreateNodeArgsPropNoNodeGUI, true);
#endif
        args->setProperty<std::string>(kCreateNodeArgsPropNodeInitialName, fixedNamePrefix.toStdString());
        args->setProperty<bool>(kCreateNodeArgsPropAllowNonUserCreatablePlugins, true);

        _imp->effectNode = app->createNode(args);
        if (!_imp->effectNode) {
            throw std::runtime_error("Rotopaint requires the plug-in " + pluginId.toStdString() + " in order to work");
        }
        _imp->nodes.push_back(_imp->effectNode);
        assert(_imp->effectNode);
    }

    if (type == eRotoStrokeTypeBlur) {
        assert(isStroke);
        // Link effect knob to size
        KnobIPtr knob = _imp->effectNode->getKnobByName(kBlurCImgParamSize);
        knob->linkTo(isStroke->getBrushEffectKnob());

    } else if ( (type == eRotoStrokeTypeClone) || (type == eRotoStrokeTypeReveal) ) {
        // Link transform knobs
        KnobIPtr translateKnob = _imp->effectNode->getKnobByName(kTransformParamTranslate);
        translateKnob->linkTo(isStroke->getBrushCloneTranslateKnob());

        KnobIPtr rotateKnob = _imp->effectNode->getKnobByName(kTransformParamRotate);
        rotateKnob->linkTo(isStroke->getBrushCloneRotateKnob());

        KnobIPtr scaleKnob = _imp->effectNode->getKnobByName(kTransformParamScale);
        scaleKnob->linkTo(isStroke->getBrushCloneScaleKnob());

        KnobIPtr uniformKnob = _imp->effectNode->getKnobByName(kTransformParamUniform);
        uniformKnob->linkTo(isStroke->getBrushCloneScaleUniformKnob());

        KnobIPtr skewxKnob = _imp->effectNode->getKnobByName(kTransformParamSkewX);
        skewxKnob->linkTo(isStroke->getBrushCloneSkewXKnob());

        KnobIPtr skewyKnob = _imp->effectNode->getKnobByName(kTransformParamSkewY);
        skewyKnob->linkTo(isStroke->getBrushCloneSkewYKnob());

        KnobIPtr skewOrderKnob = _imp->effectNode->getKnobByName(kTransformParamSkewOrder);
        skewOrderKnob->linkTo(isStroke->getBrushCloneSkewOrderKnob());

        KnobIPtr centerKnob = _imp->effectNode->getKnobByName(kTransformParamCenter);
        centerKnob->linkTo(isStroke->getBrushCloneCenterKnob());

        KnobIPtr filterKnob = _imp->effectNode->getKnobByName(kTransformParamFilter);
        filterKnob->linkTo(isStroke->getBrushCloneFilterKnob());

        KnobIPtr boKnob = _imp->effectNode->getKnobByName(kTransformParamBlackOutside);
        boKnob->linkTo(isStroke->getBrushCloneBlackOutsideKnob());


    }

    if (type == eRotoStrokeTypeSmear) {
        // For smear setup the type parameter
        KnobIPtr knob = _imp->effectNode->getKnobByName(kRotoShapeRenderNodeParamType);
        assert(knob);
        KnobChoicePtr typeChoice = toKnobChoice(knob);
        assert(typeChoice);
        typeChoice->setValue(1);
    }

    if ( (type == eRotoStrokeTypeClone) || (type == eRotoStrokeTypeReveal) || (type == eRotoStrokeTypeComp) ) {
        {
            fixedNamePrefix = baseFixedName;
            fixedNamePrefix.append( QString::fromUtf8("TimeOffset") );
            CreateNodeArgsPtr args(CreateNodeArgs::create(PLUGINID_OFX_TIMEOFFSET, rotoPaintEffect ));
            args->setProperty<bool>(kCreateNodeArgsPropVolatile, true);
#ifndef ROTO_PAINT_NODE_GRAPH_VISIBLE
            args->setProperty<bool>(kCreateNodeArgsPropNoNodeGUI, true);
#endif
            args->setProperty<std::string>(kCreateNodeArgsPropNodeInitialName, fixedNamePrefix.toStdString());

            _imp->timeOffsetNode = app->createNode(args);
            assert(_imp->timeOffsetNode);
            if (!_imp->timeOffsetNode) {
                throw std::runtime_error(tr("Rotopaint requires the plug-in %1 in order to work").arg(QString::fromUtf8(PLUGINID_OFX_TIMEOFFSET)).toStdString());
            }
            _imp->nodes.push_back(_imp->timeOffsetNode);
            // Link time offset knob
            KnobIPtr offsetKnob = _imp->timeOffsetNode->getKnobByName(kTimeOffsetParamOffset);
            offsetKnob->linkTo(_imp->timeOffset.lock());
        }

        // Do not create a framehold node for the comp item
        if (type != eRotoStrokeTypeComp) {
            fixedNamePrefix = baseFixedName;
            fixedNamePrefix.append( QString::fromUtf8("FrameHold") );
            CreateNodeArgsPtr args(CreateNodeArgs::create( PLUGINID_OFX_FRAMEHOLD, rotoPaintEffect ));
            args->setProperty<bool>(kCreateNodeArgsPropVolatile, true);
#ifndef ROTO_PAINT_NODE_GRAPH_VISIBLE
            args->setProperty<bool>(kCreateNodeArgsPropNoNodeGUI, true);
#endif
            args->setProperty<std::string>(kCreateNodeArgsPropNodeInitialName, fixedNamePrefix.toStdString());
            _imp->frameHoldNode = app->createNode(args);
            assert(_imp->frameHoldNode);
            if (!_imp->frameHoldNode) {
                throw std::runtime_error(tr("Rotopaint requires the plug-in %1 in order to work").arg(QString::fromUtf8(PLUGINID_OFX_FRAMEHOLD)).toStdString());
            }
            _imp->nodes.push_back(_imp->frameHoldNode);
            // Link frame hold first frame knob
            KnobIPtr offsetKnob = _imp->frameHoldNode->getKnobByName(kFrameHoldParamFirstFrame);
            offsetKnob->linkTo(_imp->timeOffset.lock());
        }
    }



    // Create the merge node used by any roto item
    fixedNamePrefix = baseFixedName;
    fixedNamePrefix.append( QString::fromUtf8("Merge") );

    CreateNodeArgsPtr args(CreateNodeArgs::create( PLUGINID_OFX_MERGE, rotoPaintEffect ));
    args->setProperty<bool>(kCreateNodeArgsPropVolatile, true);
#ifndef ROTO_PAINT_NODE_GRAPH_VISIBLE
    args->setProperty<bool>(kCreateNodeArgsPropNoNodeGUI, true);
#endif
    args->setProperty<std::string>(kCreateNodeArgsPropNodeInitialName, fixedNamePrefix.toStdString());

    _imp->mergeNode = app->createNode(args);
    assert(_imp->mergeNode);
    if (!_imp->mergeNode) {
        throw std::runtime_error(tr("Rotopaint requires the plug-in %1 in order to work").arg(QString::fromUtf8(PLUGINID_OFX_MERGE)).toStdString());
    }
    _imp->nodes.push_back(_imp->mergeNode);

    {
        // Link the RGBA enabled checkbox of the Rotopaint to the merge output RGBA
        KnobBoolPtr rotoPaintRGBA[4];
        KnobBoolPtr mergeRGBA[4];
        rotoPaintEffect->getEnabledChannelKnobs(&rotoPaintRGBA[0], &rotoPaintRGBA[1], &rotoPaintRGBA[2], &rotoPaintRGBA[3]);
        mergeRGBA[0] = toKnobBool(_imp->mergeNode->getKnobByName(kMergeParamOutputChannelsR));
        mergeRGBA[1] = toKnobBool(_imp->mergeNode->getKnobByName(kMergeParamOutputChannelsG));
        mergeRGBA[2] = toKnobBool(_imp->mergeNode->getKnobByName(kMergeParamOutputChannelsB));
        mergeRGBA[3] = toKnobBool(_imp->mergeNode->getKnobByName(kMergeParamOutputChannelsA));
        for (int i = 0; i < 4; ++i) {
            ignore_result(mergeRGBA[i]->linkTo(rotoPaintRGBA[i]));
        }



        // Link the compositing operator to this knob
        KnobChoicePtr mergeOp = toKnobChoice(_imp->mergeNode->getKnobByName(kMergeOFXParamOperation));
        assert(mergeOp);
        KnobChoicePtr compOp = getOperatorKnob();
        {
            bool ok = mergeOp->linkTo(compOp);
            assert(ok);
            (void)ok;
        }

        MergingFunctionEnum op;
        if ( (type == eRotoStrokeTypeDodge) || (type == eRotoStrokeTypeBurn) ) {
            op = (type == eRotoStrokeTypeDodge ? eMergeColorDodge : eMergeColorBurn);
        } else if (type == eRotoStrokeTypeSolid || type == eRotoStrokeTypeComp) {
            op = eMergeOver;
        } else {
            op = eMergeCopy;
        }

        compOp->setDefaultValueFromID(Merge::getOperatorString(op));

        // Make sure it is not serialized
        compOp->setCurrentDefaultValueAsInitialValue();

        KnobIPtr thisInvertKnob = _imp->invertKnob.lock();
        if (thisInvertKnob) {
            // Link mask invert knob
            KnobIPtr mergeMaskInvertKnob = _imp->mergeNode->getKnobByName(kMergeOFXParamInvertMask);
            mergeMaskInvertKnob->linkTo(thisInvertKnob);
        }

        // Link mix
        KnobIPtr rotoPaintMix;
        if (rotoPaintEffect->getRotoPaintNodeType() == RotoPaint::eRotoPaintTypeComp) {
            rotoPaintMix = _imp->mixKnob.lock();
        } else {
            rotoPaintMix = rotoPaintEffect->getOrCreateHostMixKnob(rotoPaintEffect->getOrCreateMainPage());
        }
        KnobIPtr mergeMix = _imp->mergeNode->getKnobByName(kMergeOFXParamMix);
        mergeMix->linkTo(rotoPaintMix);
    }

    if ( (type != eRotoStrokeTypeSolid) && (type != eRotoStrokeTypeSmear) && (type != eRotoStrokeTypeComp)) {
        // Create the mask plug-in

        {
            fixedNamePrefix = baseFixedName;
            fixedNamePrefix.append( QString::fromUtf8("Mask") );
            CreateNodeArgsPtr args(CreateNodeArgs::create( maskPluginID.toStdString(), rotoPaintEffect ));
            args->setProperty<bool>(kCreateNodeArgsPropVolatile, true);
#ifndef ROTO_PAINT_NODE_GRAPH_VISIBLE
            args->setProperty<bool>(kCreateNodeArgsPropNoNodeGUI, true);
#endif
            args->setProperty<bool>(kCreateNodeArgsPropAllowNonUserCreatablePlugins, true);
            args->setProperty<std::string>(kCreateNodeArgsPropNodeInitialName, fixedNamePrefix.toStdString());
            _imp->maskNode = app->createNode(args);
            assert(_imp->maskNode);
            if (!_imp->maskNode) {
                throw std::runtime_error(tr("Rotopaint requires the plug-in %1 in order to work").arg(maskPluginID).toStdString());
            }
            _imp->nodes.push_back(_imp->maskNode);

            {
                // For masks set the output components to alpha
                KnobIPtr knob = _imp->maskNode->getKnobByName(kRotoShapeRenderNodeParamOutputComponents);
                assert(knob);
                KnobChoicePtr typeChoice = toKnobChoice(knob);
                assert(typeChoice);
                typeChoice->setValue(1);
            }
        }
    }

#ifdef ROTOPAINT_MOTIONBLUR_USE_TIMEBLUR
    if (type == eRotoStrokeTypeSolid) {
        // For solid (Bezier/paint stroke) add a TimeBlur node right away after the RotoShapeRender node
        // so the user can add per-shape motion blur.
        fixedNamePrefix = baseFixedName;
        fixedNamePrefix.append( QString::fromUtf8("PerShapeMotionBlur") );
        CreateNodeArgsPtr args(CreateNodeArgs::create( PLUGINID_OFX_TIMEBLUR, rotoPaintEffect ));
        args->setProperty<bool>(kCreateNodeArgsPropVolatile, true);
#ifndef ROTO_PAINT_NODE_GRAPH_VISIBLE
        args->setProperty<bool>(kCreateNodeArgsPropNoNodeGUI, true);
#endif
        args->setProperty<bool>(kCreateNodeArgsPropAllowNonUserCreatablePlugins, true);
        args->setProperty<std::string>(kCreateNodeArgsPropNodeInitialName, fixedNamePrefix.toStdString());
        _imp->timeBlurNode = app->createNode(args);
        assert(_imp->timeBlurNode);
        if (!_imp->timeBlurNode) {
            throw std::runtime_error(tr("Rotopaint requires the plug-in %1 in order to work").arg(QLatin1String(PLUGINID_OFX_TIMEBLUR)).toStdString());
        }
        _imp->nodes.push_back(_imp->timeBlurNode);

        KnobIPtr divisionsKnob = _imp->timeBlurNode->getKnobByName(kTimeBlurParamDivisions);
        KnobIPtr shutterKnob = _imp->timeBlurNode->getKnobByName(kTimeBlurParamShutter);
        KnobIPtr shutterTypeKnob = _imp->timeBlurNode->getKnobByName(kTimeBlurParamShutterOffset);
        KnobIPtr shutterCustomOffsetKnob = _imp->timeBlurNode->getKnobByName(kTimeBlurParamCustomOffset);
        assert(divisionsKnob && shutterKnob && shutterTypeKnob && shutterCustomOffsetKnob);
        divisionsKnob->slaveTo(_imp->motionBlurAmount.lock());
        shutterKnob->slaveTo(_imp->motionBlurShutter.lock());
        shutterTypeKnob->slaveTo(_imp->motionBlurShutterType.lock());
        shutterCustomOffsetKnob->slaveTo(_imp->motionBlurCustomShutter.lock());

    }
#endif // ROTOPAINT_MOTIONBLUR_USE_TIMEBLUR

    // Whenever the hash of the item changes, invalidate the hash of the RotoPaint nodes and all nodes within it.
    // This needs to be done because the hash needs to be recomputed if the Solo state changes for instance?
    addHashListener(rotoPaintEffect);


    if (isStroke) {
        if (type == eRotoStrokeTypeSmear) {
            KnobDoublePtr spacingKnob = isStroke->getBrushSpacingKnob();
            assert(spacingKnob);
            spacingKnob->setValue(0.05);
        }

        setNodesThreadSafetyForRotopainting();
    }

    ///Attach this stroke to the underlying nodes used
    for (NodesList::iterator it = _imp->nodes.begin(); it != _imp->nodes.end(); ++it) {
        attachStrokeToNode(*it, rotoPaintEffect->getNode(), thisShared);
    }

    if (connectNodes) {
        refreshNodesConnections();
    }
} // RotoDrawableItem::createNodes


void
RotoDrawableItem::disconnectNodes()
{
    for (NodesList::iterator it = _imp->nodes.begin(); it != _imp->nodes.end(); ++it) {
        int maxInputs = (*it)->getMaxInputCount();
        (*it)->beginInputEdition();
        for (int i = 0; i < maxInputs; ++i) {
            (*it)->disconnectInput(i);
        }
        (*it)->endInputEdition(true);
    }
}

void
RotoDrawableItem::deactivateNodes()
{
    for (NodesList::iterator it = _imp->nodes.begin(); it != _imp->nodes.end(); ++it) {
        (*it)->deactivate(std::list< NodePtr >(), true, false, false, false);
    }
}

void
RotoDrawableItem::activateNodes()
{
    for (NodesList::iterator it = _imp->nodes.begin(); it != _imp->nodes.end(); ++it) {
        (*it)->activate(std::list< NodePtr >(), false, false);
    }
}

bool
RotoDrawableItem::onKnobValueChanged(const KnobIPtr& knob,
                        ValueChangedReasonEnum reason,
                        TimeValue time,
                        ViewSetSpec view)
{
    KnobItemsTablePtr model = getModel();
    if (!model) {
        return false;
    }
    NodePtr node = model->getNode();
    if (!node) {
        return false;
    }

    RotoPaintPtr rotoPaintEffect = toRotoPaint(node->getEffectInstance());

    // Any knob except transform center should break the multi-stroke into a new stroke
    if ( (reason == eValueChangedReasonUserEdited) && (knob->getName() != kRotoBrushCenterParam) && (knob->getName() != kRotoDrawableItemCenterParam)) {
        rotoPaintEffect->onBreakMultiStrokeTriggered();
    }
    if (knob == getActivatedKnob() || knob == getSoloKnob()) {
        // When the item is activated we must refresh the tree
        bool ret = RotoItem::onKnobValueChanged(knob, reason, time, view);
        for (NodesList::iterator it = _imp->nodes.begin(); it != _imp->nodes.end(); ++it) {
            (*it)->refreshIdentityState();
        }
        if (getIndexInParent() != -1) {
            rotoPaintEffect->refreshRotoPaintTree();
        }
        return ret;
    } else if (reason != eValueChangedReasonTimeChanged && (knob == _imp->compOperator.lock() || knob == _imp->mixKnob.lock() || knob == _imp->mergeAInputChoice.lock() || knob == _imp->mergeMaskInputChoice.lock() || knob == _imp->customRange.lock() || knob == _imp->lifeTime.lock())) {
        if (getIndexInParent() != -1) {
            rotoPaintEffect->refreshRotoPaintTree();
        }
    } else if ( (knob == _imp->timeOffsetMode.lock()) && _imp->timeOffsetNode ) {
        refreshNodesConnections();
    } else {
        return RotoItem::onKnobValueChanged(knob, reason, time, view);
    }

   
    return true;
} // RotoDrawableItem::onKnobValueChanged


NodePtr
RotoDrawableItem::getEffectNode() const
{
    return _imp->effectNode;
}

NodePtr
RotoDrawableItem::getMergeNode() const
{
    return _imp->mergeNode;
}

NodePtr
RotoDrawableItem::getTimeOffsetNode() const
{
    return _imp->timeOffsetNode;
}


NodePtr
RotoDrawableItem::getMaskNode() const
{
    return _imp->maskNode;
}


NodePtr
RotoDrawableItem::getFrameHoldNode() const
{
    return _imp->frameHoldNode;
}


void
RotoDrawableItem::refreshNodesPositions(double x, double y)
{
    _imp->mergeNode->setPosition(x, y);
    if (_imp->maskNode) {
        _imp->maskNode->setPosition(x - 100, y);
    }
    double yOffset = 100;
#ifdef ROTOPAINT_MOTIONBLUR_USE_TIMEBLUR
    if (_imp->timeBlurNode) {
        _imp->timeBlurNode->setPosition(x, y - yOffset);
        yOffset += 100;
    }
#endif

    if (_imp->effectNode) {
        _imp->effectNode->setPosition(x, y - yOffset);
        yOffset += 100;
    }
    if (_imp->timeOffsetNode) {
        if (_imp->frameHoldNode) {
            _imp->timeOffsetNode->setPosition(x - 100, y - yOffset);
            _imp->frameHoldNode->setPosition(x + 100, y - yOffset);
        } else {
            _imp->timeOffsetNode->setPosition(x, y - yOffset);
        }
    }

}

void
RotoDrawableItem::refreshNodesConnections()
{
    KnobItemsTablePtr model = getModel();
    if (!model) {
        return ;
    }
    NodePtr node = model->getNode();
    if (!node) {
        return;
    }

    RotoPaintPtr rotoPaintNode = toRotoPaint(node->getEffectInstance());
    if (!rotoPaintNode) {
        return;
    }

    RotoDrawableItemPtr previous = boost::dynamic_pointer_cast<RotoDrawableItem>(getNextNonContainerItem());

    NodePtr rotoPaintInput0 = rotoPaintNode->getInternalInputNode(0);

    // upstreamNode is the node that should be connected as the B input of the merge node of this item.
    // If there is a previous item, this is the previous item's merge node, otherwise this is the RotoPaint node input 0
    NodePtr upstreamNode = previous ? previous->getMergeNode() : rotoPaintInput0;

    RotoStrokeType type = getBrushType();

    NodePtr mergeInputA, mergeInputB;

    if (type == eRotoStrokeTypeComp) {
        // For comp items, internal tree goes like this:
        /*    (A) -- Time Offset ---- user selected merge A input (from the knob)
            /
            Merge
            \ 
             (B) --------------------- upstream node
         */

        assert(_imp->timeOffsetNode);
        KnobChoicePtr mergeAKnob = _imp->mergeAInputChoice.lock();

        int mergeAInputChoice_i = mergeAKnob->getValue();

        NodePtr mergeInputAUpstreamNode;
        if (mergeAInputChoice_i == 0) {
            mergeInputAUpstreamNode = upstreamNode;
        } else {
            ChoiceOption inputAName = mergeAKnob->getActiveEntry();
            int inputNb = QString::fromUtf8(inputAName.id.c_str()).toInt();
            mergeInputAUpstreamNode = rotoPaintNode->getInternalInputNode(inputNb);
        }

        mergeInputB = upstreamNode;

        if (mergeInputAUpstreamNode) {
            mergeInputA = _imp->timeOffsetNode;
            _imp->timeOffsetNode->swapInput(mergeInputAUpstreamNode, 0);

        } else {
            // No node upstream, make the merge be a pass-through of input B (upstreamNode)
            mergeInputA = upstreamNode;
            _imp->timeOffsetNode->disconnectInput(0);

        }
    } else if ( _imp->effectNode && ( type != eRotoStrokeTypeEraser) ) {
        // Base case that handles the following types:
        // Solid, Blur, Sharpen, Clone, Reveal, Smear

        /*
           internal node tree for this item  goes like this:
                  (A) - <Optional TimeBlur node> - Effect - <Optional Time Node>------- Reveal input (Reveal/Clone) or Upstream Node otherwise
                /
                Merge
                \
                  (B) ------------------------------------Upstream Node

         */

        // This is the node that we should connect to the A source upstream.
        NodePtr effectInput;
        if (!_imp->timeOffsetNode) {
            effectInput = _imp->effectNode;
        } else {

            // If there's a time-offset, use it prior to the effect.
            KnobChoicePtr timeOffsetModeKnob = _imp->timeOffsetMode.lock();
            int timeOffsetMode_i = 0;
            if (timeOffsetModeKnob) {
                timeOffsetMode_i = timeOffsetModeKnob->getValue();
            }
            if (timeOffsetMode_i == 0) {
                //relative
                effectInput = _imp->timeOffsetNode;
            } else {
                effectInput = _imp->frameHoldNode;
            }
            _imp->effectNode->swapInput(effectInput, 0);

        }


#ifdef ROTOPAINT_MOTIONBLUR_USE_TIMEBLUR
        RotoMotionBlurModeEnum mbType = (RotoMotionBlurModeEnum)rotoPaintNode->getMotionBlurTypeKnob()->getValue();
        if (_imp->timeBlurNode) {
            _imp->timeBlurNode->swapInput(mbType == eRotoMotionBlurModePerShape ? _imp->effectNode : NodePtr(), 0);
        }

        if (mbType == eRotoMotionBlurModePerShape && _imp->timeBlurNode) {
            mergeInputA = _imp->timeBlurNode;
        } else
#endif
        {
            mergeInputA = _imp->effectNode;
        }
        mergeInputB = upstreamNode;

        // Determine what we should connect to upstream of the A input
        NodePtr mergeAUpstreamInput;
        if (type != eRotoStrokeTypeSolid) {
            mergeAUpstreamInput = upstreamNode;
        } else if ((type == eRotoStrokeTypeReveal) || ( type == eRotoStrokeTypeClone)) {
            KnobChoicePtr mergeAKnob = _imp->mergeAInputChoice.lock();
            int reveal_i = mergeAKnob->getValue();
            if (reveal_i == 0) {
                mergeAUpstreamInput = upstreamNode;
            } else {
                // For reveal & clone, the user can select a RotoPaint node's input.
                // Find an input of the RotoPaint node with the given input label

                ChoiceOption inputAName = mergeAKnob->getActiveEntry();
                int inputNb = QString::fromUtf8(inputAName.id.c_str()).toInt();
                mergeAUpstreamInput = rotoPaintNode->getInternalInputNode(inputNb);

            }
        }

        effectInput->swapInput(mergeAUpstreamInput, 0);

    } else {
        assert(type == eRotoStrokeTypeEraser ||
               type == eRotoStrokeTypeDodge ||
               type == eRotoStrokeTypeBurn);


        if (type == eRotoStrokeTypeEraser) {
            /*
               Tree for this effect goes like this:
                        (A) Constant or RotoPaint bg input
                    /
               Merge
                    \
                        (B) --------------------Upstream Node

             */


            NodePtr eraserInput = rotoPaintInput0 ? rotoPaintInput0 : _imp->effectNode;
            mergeInputA = eraserInput;
            mergeInputB = upstreamNode;

        }  else if ( (type == eRotoStrokeTypeDodge) || (type == eRotoStrokeTypeBurn) ) {
            /*
               Tree for this effect goes like this:
                    (A) Upstream Node
                /
               Merge (Dodge/Burn)
                \
                    (B) Upstream Node

             */
            mergeInputA = mergeInputB = upstreamNode;

        } else {
            //unhandled case
            assert(false);
        }
    } //if (_imp->effectNode &&  type != eRotoStrokeTypeEraser)

    // If the tree is concatenated, do not use this merge node, instead
    // use the global merge node at the bottom of the RotoPaint tree.
    // Otherwise connect the merge node B input to the effect.
    //if (!isTreeConcatenated) {

        // For the merge node,
        // A input index is 1
        // B input index is 0
        _imp->mergeNode->swapInput(mergeInputA, 1); // A
        _imp->mergeNode->swapInput(mergeInputB, 0); // B

    //}
    /*else {

        _imp->mergeNode->disconnectInput(0);
        _imp->mergeNode->disconnectInput(1);
        _imp->mergeNode->disconnectInput(2);
    }*/

    // Connect to a mask if needed
    if (_imp->maskNode) {
        //Connect the merge node mask to the mask node
        _imp->mergeNode->swapInput(_imp->maskNode, 2);

    } else if (type == eRotoStrokeTypeComp) {
        KnobChoicePtr knob = _imp->mergeMaskInputChoice.lock();
        int maskInput_i = knob->getValue();
        NodePtr maskInputNode;
        if (maskInput_i > 0) {

            ChoiceOption maskInputName = knob->getActiveEntry();
            int inputNb = QString::fromUtf8(maskInputName.id.c_str()).toInt();
            maskInputNode = rotoPaintNode->getInternalInputNode(inputNb);
        }
        //Connect the merge node mask to the mask node
        _imp->mergeNode->swapInput(maskInputNode, 2);

    }
    
} // RotoDrawableItem::refreshNodesConnections

void
RotoDrawableItem::resetNodesThreadSafety()
{
    for (NodesList::iterator it = _imp->nodes.begin(); it != _imp->nodes.end(); ++it) {
        (*it)->getEffectInstance()->revertToPluginThreadSafety();
    }

    KnobItemsTablePtr model = getModel();
    if (!model) {
        return ;
    }
    NodePtr node = model->getNode();
    if (!node) {
        return;
    }
    RotoPaintPtr rotoPaintNode = toRotoPaint(node->getEffectInstance());
    if (!rotoPaintNode) {
        return;
    }
    node->getEffectInstance()->revertToPluginThreadSafety();
}


bool
RotoDrawableItem::isActivated(TimeValue time, ViewIdx view) const
{
    if ( !isGloballyActivatedRecursive() ) {
        return false;
    }
    try {
        KnobChoicePtr lifeTimeKnob = _imp->lifeTime.lock();
        if (!lifeTimeKnob) {
            return true;
        }
        RotoPaintItemLifeTimeTypeEnum lifetime = (RotoPaintItemLifeTimeTypeEnum)lifeTimeKnob->getValue();

        // The time in parameter may be a float if e.g a TimeBlur node is in the graph. As a result of this
        // the lifetime frame would not exactly match the given time. Instead round the time to the closest integer.
        int roundedTime = std::floor(time + 0.5);
        switch (lifetime) {
            case eRotoPaintItemLifeTimeTypeAll:
                return true;
            case eRotoPaintItemLifeTimeTypeSingle:
                return roundedTime == _imp->lifeTimeFrame.lock()->getValue(DimIdx(0),view);
            case eRotoPaintItemLifeTimeTypeFromStart:
                return roundedTime <= _imp->lifeTimeFrame.lock()->getValue(DimIdx(0),view);
            case eRotoPaintItemLifeTimeTypeToEnd:
                return roundedTime >= _imp->lifeTimeFrame.lock()->getValue(DimIdx(0),view);
            case eRotoPaintItemLifeTimeTypeCustom:
                return _imp->customRange.lock()->getValueAtTime(time, DimIdx(0), view);
        }

    } catch (std::runtime_error) {
    }
    return false;
}

std::vector<RangeD>
RotoDrawableItem::getActivatedRanges(ViewIdx view) const
{
    std::vector<RangeD> ret;
    RotoPaintItemLifeTimeTypeEnum lifetime = (RotoPaintItemLifeTimeTypeEnum)_imp->lifeTime.lock()->getValue();
    switch (lifetime) {
        case eRotoPaintItemLifeTimeTypeAll: {
            RangeD r = {INT_MIN, INT_MAX};
            ret.push_back(r);
            break;
        }
        case eRotoPaintItemLifeTimeTypeSingle: {
            double frame = _imp->lifeTimeFrame.lock()->getValue(DimIdx(0),view);
            RangeD r = {frame, frame};
            ret.push_back(r);
            break;
        }
        case eRotoPaintItemLifeTimeTypeFromStart: {
            double frame = _imp->lifeTimeFrame.lock()->getValue(DimIdx(0),view);
            RangeD r = {frame, INT_MAX};
            ret.push_back(r);
            break;
        }
        case eRotoPaintItemLifeTimeTypeToEnd: {
            double frame = _imp->lifeTimeFrame.lock()->getValue(DimIdx(0),view);
            RangeD r = {INT_MIN, frame};
            ret.push_back(r);
            break;
        }
        case eRotoPaintItemLifeTimeTypeCustom: {
            KnobBoolPtr customRangeKnob = _imp->customRange.lock();
            CurvePtr curve = customRangeKnob->getAnimationCurve(view, DimIdx(0));
            if (curve->isAnimated()) {
                assert(curve);
                KeyFrameSet keys = curve->getKeyFrames_mt_safe();
                assert(!keys.empty());
                bool rangeOpened = keys.begin()->getValue() > 0;
                RangeD r = {INT_MIN, INT_MAX};
                for (KeyFrameSet::const_iterator it = keys.begin(); it != keys.end(); ++it) {
                    if (it->getValue() > 0) {
                        if (!rangeOpened) {
                            r.min = it->getTime();
                            rangeOpened = true;
                        }
                    } else {
                        if (rangeOpened) {
                            r.max = it->getTime();
                            rangeOpened = false;
                            ret.push_back(r);
                        }
                    }
                }
                if (rangeOpened) {
                    r.max = INT_MAX;
                    ret.push_back(r);
                }
            } else {
                bool activated = customRangeKnob->getValue();
                if (activated) {
                    RangeD r = {INT_MIN, INT_MAX};
                    ret.push_back(r);
                }
            }
            break;
        }

    }
    return ret;
} // getActivatedRanges

void
RotoDrawableItem::getDefaultOverlayColor(double *r, double *g, double *b)
{
    *r = 0.85164;
    *g = 0.196936;
    *b = 0.196936;
}


KnobBoolPtr RotoDrawableItem::getCustomRangeKnob() const
{
    return _imp->customRange.lock();
}

KnobDoublePtr RotoDrawableItem::getOpacityKnob() const
{
    return _imp->opacity.lock();
}

KnobButtonPtr RotoDrawableItem::getInvertedKnob() const
{
    return _imp->invertKnob.lock();
}

KnobChoicePtr RotoDrawableItem::getOperatorKnob() const
{
    return _imp->compOperator.lock();
}

KnobColorPtr RotoDrawableItem::getColorKnob() const
{
    return _imp->color.lock();
}

KnobColorPtr
RotoDrawableItem::getOverlayColorKnob() const
{
    return _imp->overlayColor.lock();
}

KnobIntPtr
RotoDrawableItem::getTimeOffsetKnob() const
{
    return _imp->timeOffset.lock();
}

KnobChoicePtr
RotoDrawableItem::getTimeOffsetModeKnob() const
{
    return _imp->timeOffsetMode.lock();
}

KnobChoicePtr
RotoDrawableItem::getMergeInputAChoiceKnob() const
{
    return _imp->mergeAInputChoice.lock();
}

KnobChoicePtr
RotoDrawableItem::getMergeMaskChoiceKnob() const
{
    return _imp->mergeMaskInputChoice.lock();
}

KnobDoublePtr
RotoDrawableItem::getMixKnob() const
{
    return _imp->mixKnob.lock();
}

KnobDoublePtr
RotoDrawableItem::getCenterKnob() const
{
    return _imp->center.lock();
}

KnobIntPtr
RotoDrawableItem::getLifeTimeFrameKnob() const
{
    return _imp->lifeTimeFrame.lock();
}

KnobDoublePtr
RotoDrawableItem::getBrushSizeKnob() const
{
    return _imp->brushSize.lock();
}

KnobDoublePtr
RotoDrawableItem::getBrushHardnessKnob() const
{
    return _imp->brushHardness.lock();
}

KnobDoublePtr
RotoDrawableItem::getBrushSpacingKnob() const
{
    return _imp->brushSpacing.lock();
}

KnobDoublePtr
RotoDrawableItem::getBrushVisiblePortionKnob() const
{
    return _imp->visiblePortion.lock();
}


void
RotoDrawableItem::setKeyframeOnAllTransformParameters(TimeValue time)
{
    KnobDoublePtr translate = _imp->translate.lock();
    if (translate) {
        translate->setValueAtTime(time, translate->getValue(DimIdx(0)), ViewSetSpec::all(), DimIdx(0));
        translate->setValueAtTime(time, translate->getValue(DimIdx(1)), ViewSetSpec::all(), DimIdx(1));
    }

    KnobDoublePtr scale = _imp->scale.lock();
    if (scale) {
        scale->setValueAtTime(time, scale->getValue(DimIdx(0)), ViewSetSpec::all(), DimIdx(0));
        scale->setValueAtTime(time, scale->getValue(DimIdx(1)), ViewSetSpec::all(), DimIdx(1));
    }

    KnobDoublePtr rotate = _imp->rotate.lock();
    if (rotate) {
        rotate->setValueAtTime(time, rotate->getValue(DimIdx(0)), ViewSetSpec::all(), DimIdx(0));
    }

    KnobDoublePtr skewX = _imp->skewX.lock();
    KnobDoublePtr skewY = _imp->skewY.lock();
    if (skewX) {
        skewX->setValueAtTime(time, skewX->getValue(DimIdx(0)), ViewSetSpec::all(), DimIdx(0));
    }
    if (skewY) {
        skewY->setValueAtTime(time, skewY->getValue(DimIdx(0)), ViewSetSpec::all(), DimIdx(0));
    }
}


void
RotoDrawableItem::getTransformAtTime(TimeValue time,
                                     ViewIdx view,
                                     Transform::Matrix3x3* matrix) const
{
    KnobDoublePtr translate = _imp->translate.lock();
    if (!translate) {
        matrix->setIdentity();
        return;
    }
    KnobDoublePtr rotate = _imp->rotate.lock();
    KnobBoolPtr scaleUniform = _imp->scaleUniform.lock();
    KnobDoublePtr scale = _imp->scale.lock();
    KnobDoublePtr skewXKnob = _imp->skewX.lock();
    KnobDoublePtr skewYKnob = _imp->skewY.lock();
    KnobDoublePtr centerKnob = _imp->center.lock();
    KnobDoublePtr extraMatrix = _imp->extraMatrix.lock();
    KnobChoicePtr skewOrder = _imp->skewOrder.lock();

    double tx = translate->getValueAtTime(time, DimIdx(0), view);
    double ty = translate->getValueAtTime(time, DimIdx(1), view);
    double sx = scale->getValueAtTime(time, DimIdx(0), view);
    double sy = scaleUniform->getValueAtTime(time) ? sx : scale->getValueAtTime(time, DimIdx(1), view);
    double skewX = skewXKnob->getValueAtTime(time, DimIdx(0), view);
    double skewY = skewYKnob->getValueAtTime(time, DimIdx(0), view);
    double rot = rotate->getValueAtTime(time, DimIdx(0), view);

    rot = Transform::toRadians(rot);
    double centerX = centerKnob->getValueAtTime(time, DimIdx(0), view);
    double centerY = centerKnob->getValueAtTime(time, DimIdx(1), view);
    bool skewOrderYX = skewOrder->getValueAtTime(time) == 1;
    *matrix = Transform::matTransformCanonical(tx, ty, sx, sy, skewX, skewY, skewOrderYX, rot, centerX, centerY);

    Transform::Matrix3x3 extraMat;
    extraMat.a = extraMatrix->getValueAtTime(time, DimIdx(0), view); extraMat.b = extraMatrix->getValueAtTime(time, DimIdx(1), view); extraMat.c = extraMatrix->getValueAtTime(time, DimIdx(2), view);
    extraMat.d = extraMatrix->getValueAtTime(time, DimIdx(3), view); extraMat.e = extraMatrix->getValueAtTime(time, DimIdx(4), view); extraMat.f = extraMatrix->getValueAtTime(time, DimIdx(5), view);
    extraMat.g = extraMatrix->getValueAtTime(time, DimIdx(6), view); extraMat.h = extraMatrix->getValueAtTime(time, DimIdx(7), view); extraMat.i = extraMatrix->getValueAtTime(time, DimIdx(8), view);
    *matrix = Transform::matMul(*matrix, extraMat);
}


void
RotoDrawableItem::setExtraMatrix(bool setKeyframe,
                                 TimeValue time,
                                 ViewSetSpec view,
                                 const Transform::Matrix3x3& mat)
{
    KnobDoublePtr extraMatrix = _imp->extraMatrix.lock();
    if (!extraMatrix) {
        return;
    }
    extraMatrix->beginChanges();
    if (setKeyframe) {
        std::vector<double> matValues(9);
        memcpy(&matValues[0], &mat.a, 9 * sizeof(double));
        extraMatrix->setValueAtTime(time, mat.a, view, DimIdx(0)); extraMatrix->setValueAtTime(time, mat.b, view, DimIdx(1)); extraMatrix->setValueAtTime(time, mat.c, view, DimIdx(2));
        extraMatrix->setValueAtTime(time, mat.d, view, DimIdx(3)); extraMatrix->setValueAtTime(time, mat.e, view, DimIdx(4)); extraMatrix->setValueAtTime(time, mat.f, view, DimIdx(5));
        extraMatrix->setValueAtTime(time, mat.g, view, DimIdx(6)); extraMatrix->setValueAtTime(time, mat.h, view, DimIdx(7)); extraMatrix->setValueAtTime(time, mat.i, view, DimIdx(8));
    } else {
        extraMatrix->setValue(mat.a, view, DimIdx(0)); extraMatrix->setValue(mat.b, view, DimIdx(1)); extraMatrix->setValue(mat.c, view, DimIdx(2));
        extraMatrix->setValue(mat.d, view, DimIdx(3)); extraMatrix->setValue(mat.e, view, DimIdx(4)); extraMatrix->setValue(mat.f, view, DimIdx(5));
        extraMatrix->setValue(mat.g, view, DimIdx(6)); extraMatrix->setValue(mat.h, view, DimIdx(7)); extraMatrix->setValue(mat.i, view, DimIdx(8));
    }
    extraMatrix->endChanges();
}

void
RotoDrawableItem::resetTransformCenter()
{
    KnobDoublePtr centerKnob = _imp->center.lock();
    if (!centerKnob) {
        return;
    }
    TimeValue time(getApp()->getTimeLine()->currentFrame());
    RectD bbox =  getBoundingBox(time, ViewIdx(0));


    centerKnob->beginChanges();

    //centerKnob->unSlave(DimSpec::all(), ViewSetSpec::all(), false);
    centerKnob->removeAnimation(ViewSetSpec::all(), DimSpec::all(), eValueChangedReasonUserEdited);
    
    std::vector<double> values(2);
    values[0] = (bbox.x1 + bbox.x2) / 2.;
    values[1] = (bbox.y1 + bbox.y2) / 2.;
    centerKnob->setValueAcrossDimensions(values);
    /*KnobIPtr masterKnob = linkData.masterKnob.lock();
    if (masterKnob) {
        centerKnob->slaveTo(masterKnob, DimSpec::all(), DimSpec::all(), view, view);
    }*/
    centerKnob->endChanges();
}


void
RotoDrawableItem::onItemRemovedFromModel()
{
    // Disconnect this item nodes from the other nodes in the rotopaint tree
    disconnectNodes();

    KnobItemsTablePtr model = getModel();
    if (!model) {
        return;
    }
    NodePtr node = model->getNode();
    if (!node) {
        return;
    }
    RotoPaintPtr isRotopaint = toRotoPaint(node->getEffectInstance());
    isRotopaint->refreshRotoPaintTree();
}

void
RotoDrawableItem::onItemInsertedInModel()
{
    KnobItemsTablePtr model = getModel();
    if (!model) {
        return;
    }
    NodePtr node = model->getNode();
    if (!node) {
        return;
    }
    RotoPaintPtr isRotopaint = toRotoPaint(node->getEffectInstance());
    isRotopaint->refreshRotoPaintTree();

}

void
RotoDrawableItem::getMotionBlurSettings(const TimeValue time,
                                        ViewIdx view,
                                        RangeD* range,
                                        int* divisions) const
{
    range->min = time;
    range->max = time;
    *divisions = 1;

#ifndef ROTOPAINT_MOTIONBLUR_USE_TIMEBLUR
    KnobItemsTablePtr model = getModel();
    if (!model) {
        return ;
    }
    NodePtr node = model->getNode();
    if (!node) {
        return;
    }

    RotoPaintPtr rotoPaintNode = toRotoPaint(node->getEffectInstance());
    if (!rotoPaintNode) {
        return;
    }

    RotoMotionBlurModeEnum mbType = (RotoMotionBlurModeEnum)rotoPaintNode->getMotionBlurTypeKnob()->getValue();
    if (mbType != eRotoMotionBlurModePerShape) {
        return;
    }

    KnobIntPtr motionBlurAmountKnob = _imp->motionBlurAmount.lock();
    if (!motionBlurAmountKnob) {
        return;
    }

    *divisions = motionBlurAmountKnob->getValueAtTime(time, DimIdx(0), view);

    KnobDoublePtr shutterKnob = _imp->motionBlurShutter.lock();
    assert(shutterKnob);
    double shutterInterval = shutterKnob->getValueAtTime(time, DimIdx(0), view);

    int shutterType_i = _imp->motionBlurShutterType.lock()->getValueAtTime(time, DimIdx(0), view);

    switch (shutterType_i) {
        case 0: // centered
            range->min = time - shutterInterval / 2;
            range->max = time + shutterInterval / 2;
            break;
        case 1: // start
            range->min = time;
            range->max = time + shutterInterval;
            break;
        case 2: // end
            range->min = time - shutterInterval;
            range->max = time;
            break;
        case 3: // custom
        {
            double shutterCustomOffset = _imp->motionBlurCustomShutter.lock()->getValueAtTime(time, DimIdx(0), view);
            range->min = time + shutterCustomOffset;
            range->max = time + shutterCustomOffset + shutterInterval;
        }   break;
        default:
            assert(false);
            range->min = time;
            range->max = time;
            break;
    }

#endif // #ifdef ROTOPAINT_MOTIONBLUR_USE_TIMEBLUR
    
}

RectD
CompNodeItem::getBoundingBox(TimeValue /*time*/, ViewIdx /*view*/) const
{
    // Not useful since we don't render any mask
    return RectD();
}

std::string
CompNodeItem::getBaseItemName() const
{
    return std::string(kRotoCompItemBaseName);
}

std::string
CompNodeItem::getSerializationClassName() const
{
    return kSerializationCompLayerTag;
}

void
RotoDrawableItem::fetchRenderCloneKnobs()
{

    RotoItem::fetchRenderCloneKnobs();

    RotoStrokeType type = getBrushType();
    RotoStrokeItem* isStroke = dynamic_cast<RotoStrokeItem*>(this);
    Bezier* isBezier = dynamic_cast<Bezier*>(this);
    if (type == eRotoStrokeTypeSolid) {
        _imp->opacity = getKnobByNameAndType<KnobDouble>(kRotoOpacityParam);
    }
    _imp->lifeTime = getKnobByNameAndType<KnobChoice>(kRotoDrawableItemLifeTimeParam);
    _imp->lifeTimeFrame = getKnobByNameAndType<KnobInt>(kRotoDrawableItemLifeTimeFrameParam);
    _imp->customRange = getKnobByNameAndType<KnobBool>(kRotoLifeTimeCustomRangeParam);

    if (type != eRotoStrokeTypeComp) {
        _imp->overlayColor  = getKnobByNameAndType<KnobColor>(kRotoOverlayColor);
    }

    _imp->compOperator = getKnobByNameAndType<KnobChoice>(kRotoCompOperatorParam);

    // Item types that output a mask may not have an invert parameter
    if (type != eRotoStrokeTypeSolid &&
        type != eRotoStrokeTypeSmear) {

            _imp->invertKnob  = getKnobByNameAndType<KnobButton>(kRotoInvertedParam);
    }
    if (type == eRotoStrokeTypeSolid) {
        _imp->color = createDuplicateOfTableKnob<KnobColor>(kRotoColorParam);
    }

    // Brush: only for strokes or open beziers
    if (isStroke || (isBezier && isBezier->isOpenBezier())) {
        _imp->brushSize = getKnobByNameAndType<KnobDouble>(kRotoBrushSizeParam);
        _imp->brushSpacing = getKnobByNameAndType<KnobDouble>(kRotoBrushSpacingParam);
        _imp->brushHardness = getKnobByNameAndType<KnobDouble>(kRotoBrushHardnessParam);
        _imp->visiblePortion = getKnobByNameAndType<KnobDouble>(kRotoBrushVisiblePortionParam);
    }


    // The comp item doesn't have a vector graphics mask hence cannot have a transform on it
    if (type != eRotoStrokeTypeComp) {
        // Transform
        _imp->translate = getKnobByNameAndType<KnobDouble>(kRotoDrawableItemTranslateParam);
        _imp->rotate = getKnobByNameAndType<KnobDouble>(kRotoDrawableItemRotateParam);
        _imp->scale = getKnobByNameAndType<KnobDouble>(kRotoDrawableItemScaleParam);
        _imp->scaleUniform = getKnobByNameAndType<KnobBool>(kRotoDrawableItemScaleUniformParam);
        _imp->skewX = getKnobByNameAndType<KnobDouble>(kRotoDrawableItemSkewXParam);
        _imp->skewY = getKnobByNameAndType<KnobDouble>(kRotoDrawableItemSkewYParam);
        _imp->skewOrder = getKnobByNameAndType<KnobChoice>(kRotoDrawableItemSkewOrderParam);
        _imp->center = getKnobByNameAndType<KnobDouble>(kRotoDrawableItemCenterParam);
        _imp->extraMatrix = getKnobByNameAndType<KnobDouble>(kRotoDrawableItemExtraMatrixParam);
    }

    if (type == eRotoStrokeTypeReveal ||
        type == eRotoStrokeTypeClone ||
        type == eRotoStrokeTypeComp) {
        _imp->mergeAInputChoice = getKnobByNameAndType<KnobChoice>(kRotoDrawableItemMergeAInputParam);
        _imp->timeOffset = getKnobByNameAndType<KnobInt>(kRotoBrushTimeOffsetParam);

        if (type != eRotoStrokeTypeComp) {
            _imp->timeOffsetMode = getKnobByNameAndType<KnobChoice>(kRotoBrushTimeOffsetModeParam);
        } else {
            _imp->mergeMaskInputChoice = getKnobByNameAndType<KnobChoice>(kRotoDrawableItemMergeMaskParam);
        }
    }

    if (type == eRotoStrokeTypeComp) {
        _imp->mixKnob = getKnobByNameAndType<KnobDouble>(kLayeredCompMixParam);
    } else {
        _imp->mixKnob = getKnobByNameAndType<KnobDouble>(kHostMixingKnobName);
    }

    if (type == eRotoStrokeTypeSolid) {
        _imp->motionBlurAmount = getKnobByNameAndType<KnobInt>(kRotoPerShapeMotionBlurParam);
        _imp->motionBlurShutter = getKnobByNameAndType<KnobDouble>(kRotoPerShapeShutterParam);
        _imp->motionBlurShutterType = getKnobByNameAndType<KnobChoice>(kRotoPerShapeShutterOffsetTypeParam);
        _imp->motionBlurCustomShutter = getKnobByNameAndType<KnobDouble>(kRotoPerShapeShutterCustomOffsetParam);
    }

} // fetchRenderCloneKnobs

void
RotoDrawableItem::initializeKnobs()
{

    RotoItem::initializeKnobs();
    
    KnobHolderPtr thisShared = shared_from_this();
    RotoStrokeItem* isStroke = dynamic_cast<RotoStrokeItem*>(this);
    Bezier* isBezier = dynamic_cast<Bezier*>(this);
    RotoStrokeType type = getBrushType();

    // Only solids may have an opacity
    if (type == eRotoStrokeTypeSolid) {
        _imp->opacity = createDuplicateOfTableKnob<KnobDouble>(kRotoOpacityParam);
    }

    // All items have a lifetime
    {
        KnobChoicePtr lifeTimeKnob = createDuplicateOfTableKnob<KnobChoice>(kRotoDrawableItemLifeTimeParam);
        _imp->lifeTime = lifeTimeKnob;
        if (isBezier) {
            lifeTimeKnob->setDefaultValue(eRotoPaintItemLifeTimeTypeAll);
        }
    }

    _imp->lifeTimeFrame = createDuplicateOfTableKnob<KnobInt>(kRotoDrawableItemLifeTimeFrameParam);
    _imp->customRange = createDuplicateOfTableKnob<KnobBool>(kRotoLifeTimeCustomRangeParam);

    // All items that have an overlay need a color knob
    if (type != eRotoStrokeTypeComp) {
        KnobColorPtr param = createKnob<KnobColor>(kRotoOverlayColor , 4);
        param->setLabel(tr(kRotoOverlayColorLabel));
        param->setHintToolTip( tr(kRotoOverlayColorHint) );
        param->setName(kRotoOverlayColor);
        std::vector<double> def(4);
        getDefaultOverlayColor(&def[0], &def[1], &def[2]);
        def[3] = 1.;
        param->setDefaultValues(def, DimIdx(0));
        _imp->overlayColor = param;
    }

    // All items have a merge node
    {
        KnobChoicePtr param = createKnob<KnobChoice>(kRotoCompOperatorParam);
        param->setLabel(tr(kRotoCompOperatorParamLabel));
        param->setHintToolTip( tr(kRotoCompOperatorHint) );

        std::vector<ChoiceOption> operators;
        Merge::getOperatorStrings(&operators);
        param->populateChoices(operators);
        param->setDefaultValueFromID( Merge::getOperatorString(eMergeOver) );
        _imp->compOperator = param;
    }

    // Item types that output a mask may not have an invert parameter
    if (type != eRotoStrokeTypeSolid &&
        type != eRotoStrokeTypeSmear) {
        {
            KnobButtonPtr param = createKnob<KnobButton>(kRotoInvertedParam);
            param->setHintToolTip( tr(kRotoInvertedHint) );
            param->setLabel(tr(kRotoInvertedParamLabel));
            param->setCheckable(true);
            param->setDefaultValue(false);
            param->setIconLabel("Images/inverted.png", true);
            param->setIconLabel("Images/uninverted.png", false);
            _imp->invertKnob = param;
        }
    }

    // Color is only useful for solids
    if (type == eRotoStrokeTypeSolid) {
        _imp->color = createDuplicateOfTableKnob<KnobColor>(kRotoColorParam);
    }



    // Brush: only for strokes or open beziers
    if (isStroke || (isBezier && isBezier->isOpenBezier())) {
        _imp->brushSize = createDuplicateOfTableKnob<KnobDouble>(kRotoBrushSizeParam);
        _imp->brushSpacing = createDuplicateOfTableKnob<KnobDouble>(kRotoBrushSpacingParam);
        _imp->brushHardness = createDuplicateOfTableKnob<KnobDouble>(kRotoBrushHardnessParam);
        _imp->visiblePortion = createDuplicateOfTableKnob<KnobDouble>(kRotoBrushVisiblePortionParam);
    }
  

    // The comp item doesn't have a vector graphics mask hence cannot have a transform on it
    if (type != eRotoStrokeTypeComp) {
        // Transform
        _imp->translate = createDuplicateOfTableKnob<KnobDouble>(kRotoDrawableItemTranslateParam);
        _imp->rotate = createDuplicateOfTableKnob<KnobDouble>(kRotoDrawableItemRotateParam);
        _imp->scale = createDuplicateOfTableKnob<KnobDouble>(kRotoDrawableItemScaleParam);
        _imp->scaleUniform = createDuplicateOfTableKnob<KnobBool>(kRotoDrawableItemScaleUniformParam);
        _imp->skewX = createDuplicateOfTableKnob<KnobDouble>(kRotoDrawableItemSkewXParam);
        _imp->skewY = createDuplicateOfTableKnob<KnobDouble>(kRotoDrawableItemSkewYParam);
        _imp->skewOrder = createDuplicateOfTableKnob<KnobChoice>(kRotoDrawableItemSkewOrderParam);
        _imp->center = createDuplicateOfTableKnob<KnobDouble>(kRotoDrawableItemCenterParam);
        _imp->extraMatrix = createDuplicateOfTableKnob<KnobDouble>(kRotoDrawableItemExtraMatrixParam);
    }


    if (type == eRotoStrokeTypeReveal ||
        type == eRotoStrokeTypeClone ||
        type == eRotoStrokeTypeComp) {
        // Source control

        {
            KnobChoicePtr param = createKnob<KnobChoice>(kRotoDrawableItemMergeAInputParam);
            param->setLabel(tr(kRotoDrawableItemMergeAInputParamLabel));
            param->setHintToolTip( tr(type == eRotoStrokeTypeComp ? kRotoDrawableItemMergeAInputParamHint_CompNode : kRotoDrawableItemMergeAInputParamHint_RotoPaint) );
            param->setDefaultValue(0);
            param->setAddNewLine(false);
            _imp->mergeAInputChoice = param;
        }

        {
            KnobIntPtr param = createKnob<KnobInt>(kRotoBrushTimeOffsetParam);
            param->setLabel(tr(kRotoBrushTimeOffsetParamLabel));
            param->setHintToolTip( tr(type == eRotoStrokeTypeComp ? kRotoBrushTimeOffsetParamHint_Comp : kRotoBrushTimeOffsetParamHint_Clone) );
            _imp->timeOffset = param;
        }
        if (type != eRotoStrokeTypeComp) {
            _imp->timeOffsetMode = createDuplicateOfTableKnob<KnobChoice>(kRotoBrushTimeOffsetModeParam);
        } else {
            {
                KnobChoicePtr param = createKnob<KnobChoice>(kRotoDrawableItemMergeMaskParam);
                param->setLabel(tr(kRotoDrawableItemMergeMaskParamLabel));
                param->setHintToolTip( tr(kRotoDrawableItemMergeMaskParamHint) );
                param->setDefaultValue(0);
                param->setAddNewLine(false);
                _imp->mergeMaskInputChoice = param;
            }
        }
    }

    if (type == eRotoStrokeTypeComp) {
        _imp->mixKnob = createDuplicateOfTableKnob<KnobDouble>(kLayeredCompMixParam);
    } else {
        _imp->mixKnob = createDuplicateOfTableKnob<KnobDouble>(kHostMixingKnobName);
    }

    if (type == eRotoStrokeTypeSolid) {
        _imp->motionBlurAmount = createDuplicateOfTableKnob<KnobInt>(kRotoPerShapeMotionBlurParam);
        _imp->motionBlurShutter = createDuplicateOfTableKnob<KnobDouble>(kRotoPerShapeShutterParam);
        _imp->motionBlurShutterType = createDuplicateOfTableKnob<KnobChoice>(kRotoPerShapeShutterOffsetTypeParam);
        _imp->motionBlurCustomShutter = createDuplicateOfTableKnob<KnobDouble>(kRotoPerShapeShutterCustomOffsetParam);
    }


    createNodes();

    if (type == eRotoStrokeTypeComp) {
        addColumn(kRotoCompOperatorParam, DimIdx(0));
        addColumn(kLayeredCompMixParam, DimIdx(0));
        addColumn(kRotoDrawableItemLifeTimeParam, DimIdx(0));
        addColumn(kRotoBrushTimeOffsetParam, DimIdx(0));
        addColumn(kRotoDrawableItemMergeAInputParam, DimIdx(0));
        addColumn(kRotoInvertedParam, DimIdx(0));
        addColumn(kRotoDrawableItemMergeMaskParam, DimIdx(0));

    } else {
        addColumn(kRotoCompOperatorParam, DimIdx(0));
        addColumn(kRotoOverlayColor, DimSpec::all());
        addColumn(kRotoColorParam, DimSpec::all());
    }

} // initializeKnobs

NATRON_NAMESPACE_EXIT;

NATRON_NAMESPACE_USING;
#include "moc_RotoDrawableItem.cpp"
