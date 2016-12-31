/* ***** BEGIN LICENSE BLOCK *****
 * This file is part of Natron <http://www.natron.fr/>,
 * Copyright (C) 2016 INRIA and Alexandre Gauthier-Foichat
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

#include "TreeRender.h"

#include <set>
#include <QThread>
#include <QMutex>
#include <QTimer>
#include <QDebug>

#include "Engine/Image.h"
#include "Engine/EffectInstance.h"
#include "Engine/GPUContextPool.h"
#include "Engine/Node.h"
#include "Engine/NodeGroup.h"
#include "Engine/RotoStrokeItem.h"
#include "Engine/Settings.h"
#include "Engine/Timer.h"
#include "Engine/ThreadPool.h"
#include "Engine/TreeRenderNodeArgs.h"

// After this amount of time, if any thread identified in this render is still remaining
// that means they are stuck probably doing a long processing that cannot be aborted or in a separate thread
// that we did not spawn. Anyway, report to the user that we cannot control this thread anymore and that it may
// waste resources.
#define NATRON_ABORT_TIMEOUT_MS 5000


NATRON_NAMESPACE_ENTER;

typedef std::set<AbortableThread*> ThreadSet;



struct TreeRenderPrivate
{

    TreeRender* _publicInterface;

    // A map of the per node tree render args set on each node
    std::map<NodePtr, TreeRenderNodeArgsPtr> perNodeArgs;

    // The nodes that had thread local storage set on them
    NodesList nodes;

    // The main root of the tree from which we want the image
    NodePtr treeRoot;

    // the time to render
    TimeValue time;

    // the view to render
    ViewIdx view;

    // Rneder statistics
    RenderStatsPtr statsObject;

    // the OpenGL contexts
    OSGLContextWPtr openGLContext, cpuOpenGLContext;

    // Are we aborted ?
    QAtomicInt aborted;

    // Protects threadsForThisRender
    mutable QMutex threadsMutex;

    // A set of threads used in this render
    ThreadSet threadsForThisRender;

    // protects timerStarted, abortTimeoutTimer and ownerThread
    mutable QMutex timerMutex;

    // Used to track when a thread is stuck in an action after abort
    QTimer* abortTimeoutTimer;
    QThread* ownerThread;
    bool timerStarted;

    bool isPlayback;
    bool isDraft;
    bool byPassCache;
    bool handleNaNs;
    bool useConcatenations;


    TreeRenderPrivate(TreeRender* publicInterface)
    : _publicInterface(publicInterface)
    , perNodeArgs()
    , nodes()
    , treeRoot()
    , time(0)
    , view()
    , statsObject()
    , openGLContext()
    , cpuOpenGLContext()
    , aborted()
    , threadsMutex()
    , threadsForThisRender()
    , timerMutex()
    , abortTimeoutTimer(0)
    , ownerThread(QThread::currentThread())
    , timerStarted(false)
    , isPlayback(false)
    , isDraft(false)
    , byPassCache(false)
    , handleNaNs(true)
    , useConcatenations(true)
    {
        aborted.fetchAndStoreAcquire(0);

        abortTimeoutTimer->setSingleShot(true);
        QObject::connect( abortTimeoutTimer, SIGNAL(timeout()), publicInterface, SLOT(onAbortTimerTimeout()) );
        QObject::connect( publicInterface, SIGNAL(startTimerInOriginalThread()), publicInterface, SLOT(onStartTimerInOriginalThreadTriggered()) );
    }

    /**
     * @brief Must be called right away after the constructor to initialize the data
     * specific to this render.
     * This returns a pointer to the render data for the tree root node.
     **/
    TreeRenderNodeArgsPtr init(const TreeRender::CtorArgsPtr& inArgs, const TreeRenderPtr& publicInterface);

    /**
     * @brief Should be called before launching any call to renderRoI to optimize the render
     **/
    StatusEnum optimizeRoI(unsigned int mipMapLevel, const RectD& canonicalRoI);

    void fetchOpenGLContext(const TreeRender::CtorArgsPtr& inArgs);

    /**
     * @brief Builds the internal render tree (including this node) and all its dependencies through expressions as well (which
     * also may be recursive).
     * This function throw exceptions upon error.
     **/
    TreeRenderNodeArgsPtr buildRenderTreeRecursive(const NodePtr& node, std::set<NodePtr>* visitedNodes);

   
};



TreeRender::TreeRender()
: _imp(new TreeRenderPrivate(this))
{
}

TreeRender::~TreeRender()
{
    // post an event to delete the timer in the thread that created it
    if (_imp->abortTimeoutTimer) {
        _imp->abortTimeoutTimer->deleteLater();
    }
}

TreeRenderNodeArgsPtr
TreeRender::getNodeRenderArgs(const NodePtr& node) const
{
    if (!node) {
        return TreeRenderNodeArgsPtr();
    }
    std::map<NodePtr, TreeRenderNodeArgsPtr>::const_iterator it = _imp->perNodeArgs.find(node);
    if (it == _imp->perNodeArgs.end()) {
        return TreeRenderNodeArgsPtr();
    }
    return it->second;
}

NodePtr
TreeRender::getTreeRoot() const
{
    return _imp->treeRoot;
}

OSGLContextPtr
TreeRender::getGPUOpenGLContext() const
{
    return _imp->openGLContext.lock();
}

OSGLContextPtr
TreeRender::getCPUOpenGLContext() const
{
    return _imp->cpuOpenGLContext.lock();
}


bool
TreeRender::isAborted() const
{
    return (int)_imp->aborted > 0;
}

void
TreeRender::setAborted()
{
    int abortedValue = _imp->aborted.fetchAndAddAcquire(1);

    if (abortedValue > 0) {
        return;
    }
    bool callInSeparateThread = false;
    {
        QMutexLocker k(&_imp->timerMutex);
        _imp->timerStarted = true;
        callInSeparateThread = QThread::currentThread() != _imp->ownerThread;
    }

    // Star the timer in its owner thread, i.e the thread that created it
    if (callInSeparateThread) {
        Q_EMIT startTimerInOriginalThread();
    } else {
        onStartTimerInOriginalThreadTriggered();
    }
}

bool
TreeRender::isPlayback() const
{
    return _imp->isPlayback;
}


bool
TreeRender::isDraftRender() const
{
    return _imp->isDraft;
}

bool
TreeRender::isByPassCacheEnabled() const
{
    return _imp->byPassCache;
}


TimeValue
TreeRender::getTime() const
{
    return _imp->time;
}

ViewIdx
TreeRender::getView() const
{
    return _imp->view;
}

RenderStatsPtr
TreeRender::getStatsObject() const
{
    return _imp->statsObject;
}

void
TreeRender::registerThreadForRender(AbortableThread* thread)
{
    QMutexLocker k(&_imp->threadsMutex);

    _imp->threadsForThisRender.insert(thread);
}

bool
TreeRender::unregisterThreadForRender(AbortableThread* thread)
{
    bool ret = false;
    bool threadsEmpty = false;
    {
        QMutexLocker k(&_imp->threadsMutex);
        ThreadSet::iterator found = _imp->threadsForThisRender.find(thread);

        if ( found != _imp->threadsForThisRender.end() ) {
            _imp->threadsForThisRender.erase(found);
            ret = true;
        }
        // Stop the timer if no more threads are running for this render
        threadsEmpty = _imp->threadsForThisRender.empty();
    }

    if (threadsEmpty) {
        {
            QMutexLocker k(&_imp->timerMutex);
            if (_imp->abortTimeoutTimer) {
                _imp->timerStarted = false;
            }
        }
    }

    return ret;
}

void
TreeRender::onStartTimerInOriginalThreadTriggered()
{
    assert(QThread::currentThread() == _imp->ownerThread);
    _imp->abortTimeoutTimer->start(NATRON_ABORT_TIMEOUT_MS);
}

void
TreeRender::onAbortTimerTimeout()
{
    {
        QMutexLocker k(&_imp->timerMutex);
        assert(QThread::currentThread() == _imp->ownerThread);
        _imp->abortTimeoutTimer->deleteLater();
        _imp->abortTimeoutTimer = 0;
        if (!_imp->timerStarted) {
            // The timer was stopped
            return;
        }
    }

    // Runs in the thread that called setAborted()
    ThreadSet threads;
    {
        QMutexLocker k(&_imp->threadsMutex);
        if ( _imp->threadsForThisRender.empty() ) {
            return;
        }
        threads = _imp->threadsForThisRender;
    }
    QString timeoutStr = Timer::printAsTime(NATRON_ABORT_TIMEOUT_MS / 1000, false);
    std::stringstream ss;

    ss << tr("One or multiple render seems to not be responding anymore after numerous attempt made by %1 to abort them for the last %2.").arg ( QString::fromUtf8( NATRON_APPLICATION_NAME) ).arg(timeoutStr).toStdString() << std::endl;
    ss << tr("This is likely due to a render taking too long in a plug-in.").toStdString() << std::endl << std::endl;

    std::stringstream ssThreads;
    ssThreads << tr("List of stalled render(s):").toStdString() << std::endl << std::endl;

    bool hasAtLeastOneThreadInNodeAction = false;
    for (ThreadSet::const_iterator it = threads.begin(); it != threads.end(); ++it) {
        std::string actionName;
        NodePtr node;
        (*it)->getCurrentActionInfos(&actionName, &node);
        if (node) {
            hasAtLeastOneThreadInNodeAction = true;
            // Don't show a dialog on timeout for writers since reading/writing from/to a file may be long and most libraries don't provide write callbacks anyway
            if ( node->getEffectInstance()->isReader() || node->getEffectInstance()->isWriter() ) {
                return;
            }
            std::string nodeName, pluginId;
            nodeName = node->getFullyQualifiedName();
            pluginId = node->getPluginID();

            ssThreads << " - " << (*it)->getThreadName()  << tr(" stalled in:").toStdString() << std::endl << std::endl;

            if ( !nodeName.empty() ) {
                ssThreads << "    Node: " << nodeName << std::endl;
            }
            if ( !pluginId.empty() ) {
                ssThreads << "    Plugin: " << pluginId << std::endl;
            }
            if ( !actionName.empty() ) {
                ssThreads << "    Action: " << actionName << std::endl;
            }
            ssThreads << std::endl;
        }
    }
    ss << std::endl;

    if (!hasAtLeastOneThreadInNodeAction) {
        return;
    } else {
        ss << ssThreads.str();
    }

    // Hold a sharedptr to this because it might get destroyed before the dialog returns
    TreeRenderPtr thisShared = shared_from_this();

    if ( appPTR->isBackground() ) {
        qDebug() << ss.str().c_str();
    } else {
        ss << tr("Would you like to kill these renders?").toStdString() << std::endl << std::endl;
        ss << tr("WARNING: Killing them may not work or may leave %1 in a bad state. The application may crash or freeze as a consequence of this. It is advised to restart %1 instead.").arg( QString::fromUtf8( NATRON_APPLICATION_NAME) ).toStdString();

        std::string message = ss.str();
        StandardButtonEnum reply = Dialogs::questionDialog(tr("A Render is not responding anymore").toStdString(), ss.str(), false, StandardButtons(eStandardButtonYes | eStandardButtonNo), eStandardButtonNo);
        if (reply == eStandardButtonYes) {
            // Kill threads
            QMutexLocker k(&_imp->threadsMutex);
            for (ThreadSet::const_iterator it = _imp->threadsForThisRender.begin(); it != _imp->threadsForThisRender.end(); ++it) {
                (*it)->killThread();
            }
        }
    }
} // onAbortTimerTimeout

void
TreeRenderPrivate::fetchOpenGLContext(const TreeRender::CtorArgsPtr& inArgs)
{

    // Ensure this thread gets an OpenGL context for the render of the frame
    OSGLContextPtr glContext, cpuContext;
    if (inArgs->activeRotoDrawableItem) {

        // When painting, always use the same context since we paint over the same texture
        assert(inArgs->activeRotoDrawableItem);
        RotoStrokeItem* isStroke = dynamic_cast<RotoStrokeItem*>(inArgs->activeRotoDrawableItem.get());
        assert(isStroke);
        if (isStroke) {
            isStroke->getDrawingGLContext(&glContext, &cpuContext);
            if (!glContext && !cpuContext) {
                try {
                    glContext = appPTR->getGPUContextPool()->getOrCreateOpenGLContext(true/*retrieveLastContext*/);
                    cpuContext = appPTR->getGPUContextPool()->getOrCreateCPUOpenGLContext(true/*retrieveLastContext*/);
                    isStroke->setDrawingGLContext(glContext, cpuContext);
                } catch (const std::exception& /*e*/) {

                }
            }
        }
    } else {
        try {
            glContext = appPTR->getGPUContextPool()->getOrCreateOpenGLContext(false/*retrieveLastContext*/);
            cpuContext = appPTR->getGPUContextPool()->getOrCreateCPUOpenGLContext(false/*retrieveLastContext*/);
        } catch (const std::exception& /*e*/) {

        }
    }

    openGLContext = glContext;
    cpuOpenGLContext = cpuContext;
}



TreeRenderNodeArgsPtr
TreeRenderPrivate::buildRenderTreeRecursive(const NodePtr& node,
                                            std::set<NodePtr>* visitedNodes)
{

    assert(node);
    // Sanity check
    if ( !node || !node->isNodeCreated() ) {
        return TreeRenderNodeArgsPtr();
    }

    EffectInstancePtr effect = node->getEffectInstance();
    if (!effect) {
        return TreeRenderNodeArgsPtr();
    }

    if (visitedNodes->find(node) != visitedNodes->end()) {
        // Already visited this node
        TreeRenderNodeArgsPtr args = _publicInterface->getNodeRenderArgs(node);
        assert(args);
        return args;
    }

    // When building the render tree, the actual graph is flattened and groups no longer exist!
    assert(!dynamic_cast<NodeGroup*>(effect.get()));

    visitedNodes->insert(node);

    // Ensure this node has a render object. If this is the first time we visit this node it will create it.
    // The render object will copy and cache all knob values and inputs and anything that may change during
    // the render.
    // Since we did not make any action calls yet, we ensure that knob values remain the same throughout the render
    // as long as this object lives.
    TreeRenderNodeArgsPtr frameArgs;
    {
        std::map<NodePtr, TreeRenderNodeArgsPtr>::const_iterator foundArgs = perNodeArgs.find(node);
        if (foundArgs != perNodeArgs.end()) {
            frameArgs = foundArgs->second;
        } else {
            frameArgs = TreeRenderNodeArgs::create(_publicInterface->shared_from_this(), node);
            node->getEffectInstance()->setCurrentRender_TLS(frameArgs);
            perNodeArgs[node] = frameArgs;
        }
    }

    
    // Recurse on all inputs to ensure they are part of the tree and make the connections to this
    // node render args
    int nInputs = node->getMaxInputCount();
    for (int i = 0; i < nInputs; ++i) {
        NodePtr inputNode = node->getInput(i);
        if (!inputNode) {
            continue;
        }
        TreeRenderNodeArgsPtr inputArgs = buildRenderTreeRecursive(inputNode, visitedNodes);
        assert(inputArgs);
        frameArgs->setInputRenderArgs(i, inputArgs);
    }

    // Visit all nodes that expressions of this node knobs may rely upon so we ensure they get a proper render object
    // and a render time and view when we run the expression.
    std::set<NodePtr> expressionsDeps;
    effect->getAllExpressionDependenciesRecursive(expressionsDeps);

    for (std::set<NodePtr>::const_iterator it = expressionsDeps.begin(); it != expressionsDeps.end(); ++it) {
        buildRenderTreeRecursive(*it, visitedNodes);
    }

    return frameArgs;
} // buildRenderTreeRecursive


TreeRenderNodeArgsPtr
TreeRenderPrivate::init(const TreeRender::CtorArgsPtr& inArgs, const TreeRenderPtr& publicInterface)
{
    assert(inArgs->treeRoot);

    time = inArgs->time;
    view = inArgs->view;
    statsObject = inArgs->stats;
    treeRoot = inArgs->treeRoot;
    isPlayback = inArgs->playback;
    isDraft = inArgs->draftMode;
    byPassCache = inArgs->byPassCache;
    handleNaNs = appPTR->getCurrentSettings()->isNaNHandlingEnabled();


    // If abortable thread, set abort info on the thread, to make the render abortable faster
    AbortableThread* isAbortable = dynamic_cast<AbortableThread*>( ownerThread );
    if (isAbortable) {
        isAbortable->setCurrentRender(publicInterface);
    }

    // Fetch the OpenGL context used for the render. It will not be attached to any render thread yet.
    fetchOpenGLContext(inArgs);


    // Build the render tree
    std::set<NodePtr> visitedNodes;
    TreeRenderNodeArgsPtr rootNodeRenderArgs = buildRenderTreeRecursive(inArgs->treeRoot, &visitedNodes);
    return rootNodeRenderArgs;

} // init

RenderRoIRetCode
TreeRender::launchRender(const CtorArgsPtr& inArgs, std::map<ImageComponents, ImagePtr>* outputPlanes)
{
    TreeRenderPtr render(new TreeRender());

    TreeRenderNodeArgsPtr rootNodeRenderArgs;
    try {
        // Setup the render tree and make local copy of knob values for the render.
        // This will also set the per node render object in the TLS for OpenFX effects.
        rootNodeRenderArgs = render->_imp->init(inArgs, render);
    } catch (...) {
        return eRenderRoIRetCodeFailed;
    }

    assert(rootNodeRenderArgs);

    EffectInstancePtr effectToRender = inArgs->treeRoot->getEffectInstance();

    // Use the provided RoI, otherwise render the RoD
    RectD canonicalRoi;
    if (inArgs->canonicalRoI) {
        canonicalRoi = *inArgs->canonicalRoI;
    } else {
        GetRegionOfDefinitionResultsPtr results;
        StatusEnum stat = effectToRender->getRegionOfDefinition_public(inArgs->time, inArgs->scale, inArgs->view, rootNodeRenderArgs, &results);
        if (stat == eStatusFailed) {
            return eRenderRoIRetCodeFailed;
        }
        assert(results);
        canonicalRoi = results->getRoD();
    }

    // Use the provided components otherwise fallback on all components needed by the output
    std::list<ImageComponents> componentsToRender;
    if (inArgs->layers) {
        componentsToRender = *inArgs->layers;
    } else {
        GetComponentsResultsPtr results;
        StatusEnum stat = effectToRender->getComponents_public(inArgs->time, inArgs->view, rootNodeRenderArgs, &results);
        if (stat == eStatusFailed) {
            return eRenderRoIRetCodeFailed;
        }
        assert(results);

        std::map<int, std::list<ImageComponents> > neededInputLayers;
        std::list<ImageComponents> producedLayers, availableLayers;
        int passThroughInputNb;
        TimeValue passThroughTime;
        ViewIdx passThroughView;
        std::bitset<4> processChannels;
        bool processAll;
        results->getResults(&neededInputLayers, &producedLayers, &availableLayers, &passThroughInputNb, &passThroughTime, &passThroughView, &processChannels, &processAll);
        componentsToRender = producedLayers;
    }

    // Cycle through the tree to make sure all nodes render once with the appropriate RoI
    {

        StatusEnum stat = rootNodeRenderArgs->roiVisitFunctor(inArgs->time, inArgs->view, inArgs->scale, canonicalRoi, effectToRender);

        if (stat == eStatusFailed) {
            return eRenderRoIRetCodeFailed;
        }
    }

    double outputPar = effectToRender->getAspectRatio(rootNodeRenderArgs, -1);

    RectI pixelRoI;
    canonicalRoi.toPixelEnclosing(inArgs->scale, outputPar, &pixelRoI);

    boost::shared_ptr<EffectInstance::RenderRoIArgs> renderRoiArgs(new EffectInstance::RenderRoIArgs(inArgs->time,
                                                                                                     inArgs->view,
                                                                                                     inArgs->scale,
                                                                                                     pixelRoI,
                                                                                                     componentsToRender,
                                                                                                     effectToRender,
                                                                                                     -1,
                                                                                                     inArgs->time,
                                                                                                     rootNodeRenderArgs));
    
    EffectInstance::RenderRoIResults results;
    RenderRoIRetCode stat = effectToRender->renderRoI(*renderRoiArgs, &results);
    *outputPlanes = results.outputPlanes;
    return stat;

} // launchRender

NATRON_NAMESPACE_EXIT;
