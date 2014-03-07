//  Natron
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
/*
 *Created by Alexandre GAUTHIER-FOICHAT on 6/1/2012.
 *contact: immarespond at gmail dot com
 *
 */

#include "ProjectSerialization.h"
#include "Engine/TimeLine.h"
#include "Engine/Project.h"
#include "Engine/AppManager.h"


void ProjectSerialization::initialize(const Natron::Project* project){
    std::vector<Natron::Node*> activeNodes;
    
    const std::vector<Natron::Node*>& nodes = project->getCurrentNodes();
    for(U32 i = 0; i < nodes.size(); ++i){
        if(nodes[i]->isActivated()){
            activeNodes.push_back(nodes[i]);
        }
    }
    
    _serializedNodes.clear();
    for (U32 i = 0; i < activeNodes.size(); ++i) {
        boost::shared_ptr<NodeSerialization> state(new NodeSerialization);
        activeNodes[i]->serialize(state.get());
        _serializedNodes.push_back(state);
    }
    _availableFormats = project->getProjectFormats();
    
    const std::vector< boost::shared_ptr<Knob> >& knobs = project->getKnobs();
    for(U32 i = 0; i < knobs.size();++i){
        if(knobs[i]->isPersistent()){
            boost::shared_ptr<KnobSerialization> newKnobSer(new KnobSerialization());
            knobs[i]->save(newKnobSer.get());
            _projectKnobs.push_back(newKnobSer);
        }
    }

    _timelineLeft = project->leftBound();
    _timelineRight = project->rightBound();
    _timelineCurrent = project->currentFrame();
    
    _creationDate = project->getProjectCreationTime();
}
