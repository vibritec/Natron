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

#ifndef NODEGUII_H
#define NODEGUII_H

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <list>

#include "Global/KeySymbols.h"
#include "Global/Enums.h"
#include "Engine/HostOverlaySupport.h"
#include "Engine/UndoCommand.h"
#include "Engine/TimeValue.h"

#include "Engine/EngineFwd.h"

NATRON_NAMESPACE_ENTER;

class NodeGuiI
{
public:

    NodeGuiI() {}

    virtual ~NodeGuiI() {}

    /**
     * @brief Should destroy any GUI associated to the internal node. Memory should be recycled and widgets no longer accessible from
     * anywhere. Upon returning this function, the caller should have a shared_ptr use_count() of 1
     **/
    virtual void destroyGui() = 0;

    /**
     * @brief Returns whether the settings panel of this node is visible or not.
     **/
    virtual bool isSettingsPanelVisible() const = 0;

    /**
     * @brief Returns whether the settings panel of this node is minimized or not.
     **/
    virtual bool isSettingsPanelMinimized() const = 0;


    /**
     * @brief Set the position of the node in the nodegraph.
     **/
    virtual void setPosition(double x, double y) = 0;

    /**
     * @brief Set the size of the bounding box of the node in the nodegraph
     **/
    virtual void setSize(double w, double h) = 0;
    
    /**
     * @brief Set the colour of the node as it appears on the nodegraph.
     **/
    virtual void setColor(double r, double g, double b) = 0;

    /**
     * @brief set the suggested overlay colour
     **/
    virtual void setOverlayColor(double r, double g, double b) = 0;

    virtual bool isOverlayLocked() const = 0;
    
    /**
     * @brief Add a default viewer overlay
     **/
    virtual void addDefaultInteract(const HostOverlayKnobsPtr& knobs) = 0;
    virtual void drawHostOverlay(TimeValue time,
                                 const RenderScale& renderScale,
                                 ViewIdx view)  = 0;
    virtual bool onOverlayPenDownDefault(TimeValue time,
                                         const RenderScale& renderScale,
                                         ViewIdx view, const QPointF & viewportPos, const QPointF & pos, double pressure)  = 0;
    virtual bool onOverlayPenDoubleClickedDefault(TimeValue time,
                                                  const RenderScale& renderScale,
                                                  ViewIdx view, const QPointF & viewportPos, const QPointF & pos)  = 0;
    virtual bool onOverlayPenMotionDefault(TimeValue time,
                                           const RenderScale& renderScale,
                                           ViewIdx view, const QPointF & viewportPos, const QPointF & pos, double pressure)  = 0;
    virtual bool onOverlayPenUpDefault(TimeValue time,
                                       const RenderScale& renderScale,
                                       ViewIdx view, const QPointF & viewportPos, const QPointF & pos, double pressure)  = 0;
    virtual bool onOverlayKeyDownDefault(TimeValue time,
                                         const RenderScale& renderScale,
                                         ViewIdx view, Key key, KeyboardModifiers modifiers)  = 0;
    virtual bool onOverlayKeyUpDefault(TimeValue time,
                                       const RenderScale& renderScale,
                                       ViewIdx view, Key key, KeyboardModifiers modifiers)  = 0;
    virtual bool onOverlayKeyRepeatDefault(TimeValue time,
                                           const RenderScale& renderScale,
                                           ViewIdx view, Key key, KeyboardModifiers modifiers) = 0;
    virtual bool onOverlayFocusGainedDefault(TimeValue time,
                                             const RenderScale& renderScale,
                                             ViewIdx view) = 0;
    virtual bool onOverlayFocusLostDefault(TimeValue time,
                                           const RenderScale& renderScale,
                                           ViewIdx view) = 0;
    virtual bool hasHostOverlay() const = 0;
    virtual void setCurrentViewportForHostOverlays(OverlaySupport* viewPort) = 0;
    virtual bool hasHostOverlayForParam(const KnobIConstPtr& param) = 0;
    virtual void removePositionHostOverlay(const KnobIPtr& knob) = 0;

    virtual bool isUserSelected() const = 0;
    virtual void restoreStateAfterCreation() = 0;
    virtual void onIdentityStateChanged(int inputNb) = 0;

    /**
     * @brief Push a new undo command to the undo/redo stack associated to this node.
     * The stack takes ownership of the shared pointer, so you should not hold a strong reference to the passed pointer.
     * If no undo/redo stack is present, the command will just be redone once then destroyed.
     **/
    virtual void pushUndoCommand(const UndoCommandPtr& command) = 0;

    /**
     * @brief Set the cursor to be one of the default cursor.
     * @returns True if it successfully set the cursor, false otherwise.
     * Note: this can only be called during an overlay interact action.
     **/
    virtual void setCurrentCursor(CursorEnum defaultCursor) = 0;
    virtual bool setCurrentCursor(const QString& customCursorFilePath) = 0;

    /**
     * @brief Make up a dialog with the content of the group
     **/
    virtual void showGroupKnobAsDialog(const KnobGroupPtr& group) = 0;

    /**
     * @brief Show a dialog and ask the user to add a new ImagePlaneDesc to the effect
     **/
    virtual bool addComponentsWithDialog(const KnobChoicePtr& knob) = 0;
};

NATRON_NAMESPACE_EXIT;

#endif // NODEGUII_H
