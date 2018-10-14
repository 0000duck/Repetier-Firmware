/*
    This file is part of Repetier-Firmware.

    Repetier-Firmware is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    Repetier-Firmware is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with Repetier-Firmware.  If not, see <http://www.gnu.org/licenses/>.

*/

#include "Repetier.h"

Motion2Buffer Motion2::buffers[NUM_MOTION2_BUFFER];
volatile fast8_t Motion2::length;
fast8_t Motion2::nextActId;
Motion2Buffer* Motion2::act;
Motion1Buffer* Motion2::actM1;
int32_t Motion2::lastMotorPos[2][NUM_AXES];
fast8_t Motion2::lastMotorIdx; // index to last pos
int Motion2::advanceSteps = 0; // already included advance steps

void Motion2::init() {
    length = nextActId = 0;
    actM1 = nullptr;
    lastMotorIdx = 0;
    for (fast8_t i = 0; i < NUM_MOTION2_BUFFER; i++) {
        buffers[i].id = i;
    }
    FOR_ALL_AXES(i) {
        lastMotorPos[0][i] = 0;
        lastMotorPos[1][i] = 0;
    }
}
// Timer gets called at PREPARE_FREQUENCY so it has enough time to
// prefill data structures required by stepper interrupt. Each segment planned
// is for a 2000 / PREPARE_FREQUENCY long period of constant speed. We try to
// precompute up to 16 such tiny buffers and with the double frequency We
// should be on the safe side of never getting an underflow.
void Motion2::timer() {
    static float lastL = 0;
    // First check if can push anything into next level
    Motion3Buffer* m3 = Motion3::tryReserve();
    if (m3 == nullptr) { // no free space, do nothing until free
        return;
    }
    // Check if we need to start a new M1 buffer
    if (actM1 == nullptr) {
        if (length == NUM_MOTION2_BUFFER || Motion1::lengthUnprocessed == 0) { // buffers full
            return;
        }
        act = &buffers[nextActId++];
        // try to get M1 block for processing
        actM1 = Motion1::forward(act);
        if (actM1 == nullptr) { // nothing to do, undo and return
            nextActId--;
            //       DEBUG_MSG_FAST("fwfail ");
            return;
        }
        if (nextActId == NUM_MOTION2_BUFFER) {
            nextActId = 0;
        }
        act->motion1 = actM1;
        act->state = Motion2State::NOT_INITIALIZED;
        if (actM1->action == Motion1Action::MOVE && actM1->isCheckEndstops()) {
            // Compute number of steps required
            float pos[NUM_AXES];
            FOR_ALL_AXES(i) {
                pos[i] = actM1->start[i] + actM1->unitDir[i] * actM1->length;
            }
            PrinterType::transform(pos, act->stepsRemaining);
            int32_t* lp = lastMotorPos[lastMotorIdx];
            FOR_ALL_AXES(i) {
                act->stepsRemaining[i] = labs(act->stepsRemaining[i] - *lp);
                lp++;
            }
        }
        lastL = 0;
        // DEBUG_MSG2_FAST("new ", (int)actM1->action);
        InterruptProtectedBlock ip;
        length++;
    }
    if (actM1->action == Motion1Action::MOVE) {
        if (act->state == Motion2State::NOT_INITIALIZED) {
            act->nextState();
            lastMotorPos[lastMotorIdx][E_AXIS] = lroundf(actM1->start[E_AXIS] * Motion1::resolution[E_AXIS]);
        }
        float sFactor = 1.0;
        if (act->state == Motion2State::ACCELERATE_INIT) {
            act->state = Motion2State::ACCELERATING;
            if (VelocityProfile::start(actM1->startSpeed, actM1->feedrate, act->t1)) {
                act->nextState();
            }
            // DEBUG_MSG2_FAST("se:", VelocityProfile::segments);
            sFactor = VelocityProfile::s;
        } else if (act->state == Motion2State::ACCELERATING) {
            if (VelocityProfile::next()) {
                act->nextState();
            }
            sFactor = VelocityProfile::s;
        } else if (act->state == Motion2State::PLATEAU_INIT) {
            act->state = Motion2State::PLATEU;
            if (VelocityProfile::start(actM1->feedrate, actM1->feedrate, act->t2)) {
                act->nextState();
            }
            sFactor = VelocityProfile::s + act->s1;
        } else if (act->state == Motion2State::PLATEU) {
            if (VelocityProfile::next()) {
                act->nextState();
            }
            sFactor = VelocityProfile::s + act->s1;
        } else if (act->state == Motion2State::DECCELERATE_INIT) {
            act->state = Motion2State::DECELERATING;
            act->soff = act->s1 + act->s2;
            if (VelocityProfile::start(actM1->feedrate, actM1->endSpeed, act->t3)) {
                act->nextState();
            }
            sFactor = VelocityProfile::s + act->soff;
        } else if (act->state == Motion2State::DECELERATING) {
            if (VelocityProfile::next()) {
                act->nextState();
            }
            sFactor = VelocityProfile::s + act->soff;
        } else if (act->state == Motion2State::FINISHED) {
            // DEBUG_MSG("finished")
            m3->directions = 0;
            m3->usedAxes = 0;
            m3->stepsRemaining = 1;
            m3->last = 1;
            // Need to add this move to handle finished state correctly
            Motion3::pushReserved();
            return;
        }
        m3->last = Motion3::skipParentId == act->id;
        if (act->state == Motion2State::FINISHED) {
            // prevent rounding errors
            sFactor = actM1->length;
            m3->last = 1;
        } else {
            if (sFactor > actM1->length) {
                sFactor = actM1->length;
                m3->last = 1;
            }
        }
        // Com::printFLN("sf:", sFactor, 4);
        // Convert float position to integer motor position
        // This step catches all nonlinear behaviour from
        // acceleration profile and printer geometry
        if (sFactor < lastL) {
            Com::printFLN(PSTR("reversal:"), sFactor - lastL, 6);
        }
        lastL = sFactor;
        float pos[NUM_AXES];
        FOR_ALL_AXES(i) {
            if (actM1->axisUsed & axisBits[i]) {
                pos[i] = actM1->start[i] + sFactor * actM1->unitDir[i];
            } else {
                pos[i] = actM1->start[i];
            }
        }
        fast8_t nextMotorIdx = 1 - lastMotorIdx;
        int32_t* np = lastMotorPos[nextMotorIdx];
        int32_t* lp = lastMotorPos[lastMotorIdx];
        PrinterType::transform(pos, np);
        // Com::printFLN(PSTR("DS x="), np[0]); // TEST
        /* Com::printF(PSTR(" y="), np[1]);
        Com::printFLN(PSTR(" z="), np[2]);*/
        // Fill structures used to update bresenham
        m3->directions = 0;
        m3->usedAxes = 0;
        if ((m3->stepsRemaining = VelocityProfile::stepsPerSegment) == 0) {
            if (m3->last) { // extreme case, normally never happens
                m3->usedAxes = 0;
                m3->stepsRemaining = 1;
                // Need to add this move to handle finished state correctly
                Motion3::pushReserved();
            }
            return; // don't add empty moves
        }
        m3->errorUpdate = m3->stepsRemaining << 1;
        int* delta = m3->delta;
        uint8_t* bits = axisBits;
        FOR_ALL_AXES(i) {
            if (i == E_AXIS && (advanceSteps != 0 || actM1->eAdv != 0)) {
                // handle advance of E
                *delta = *np - *lp;
                int advTarget = VelocityProfile::f * actM1->eAdv;
                int advDiff = advTarget - advanceSteps;
                /* Com::printF("adv:", advTarget);
                Com::printF(" d:", *delta);
                Com::printF(" as:", advanceSteps);
                Com::printF(" f:", VelocityProfile::f, 2);
                Com::printFLN(" ea:", actM1->eAdv, 4); */

                *delta += advDiff;
                if (*delta > 0) { // extruding
                    /* if (*delta < 0) { // prevent reversal, add later
                        advDiff -= *delta;
                        *delta = 0;
                    } */
                    *delta <<= 1;
                    m3->directions |= *bits;
                    m3->usedAxes |= *bits;
                } else { // retracting, advDiff is always negative
                    /* int half = *delta >> 1;
                    if (half < 1) {
                        half = 1;
                    }
                    if (half < advDiff) { // last correction
                        half = advDiff;
                    }
                    advDiff = half;
                    *delta += half;*/
                    *delta = (-*delta) << 1;
                    m3->usedAxes |= *bits;
                }
                advanceSteps += advDiff;
            } else {
                if ((*delta = ((*np - *lp) << 1)) < 0) {
                    *delta = -*delta;
                    m3->usedAxes |= *bits;
                } else if (*delta != 0) {
                    m3->directions |= *bits;
                    m3->usedAxes |= *bits;
                }
            }
            m3->error[i] = -m3->stepsRemaining;
            delta++;
            np++;
            lp++;
            bits++;
        } // FOR_ALL_AXES
        lastMotorIdx = nextMotorIdx;
        m3->parentId = act->id;
        m3->checkEndstops = actM1->isCheckEndstops();
        Tool* tool = Tool::getActiveTool();
        if (tool) {
            Motion1Buffer* motion1 = act->motion1;
            m3->secondSpeed = tool->computeIntensity(VelocityProfile::f, motion1->isActiveSecondary(), motion1->secondSpeed, motion1->secondSpeedPerMMPS);
        } else {
            m3->secondSpeed = 0;
        }
        PrinterType::enableMotors(m3->usedAxes);
        if (m3->last) {
            actM1 = nullptr; // select next on next interrupt
        }
    } else if (actM1->action == Motion1Action::MOVE_STEPS) {
        if (act->state == Motion2State::NOT_INITIALIZED) {
            act->nextState();
        }
        float sFactor = 1.0;
        if (act->state == Motion2State::ACCELERATE_INIT) {
            act->state = Motion2State::ACCELERATING;
            if (VelocityProfile::start(actM1->startSpeed, actM1->feedrate, act->t1)) {
                act->nextState();
            }
            // DEBUG_MSG2_FAST("se:", VelocityProfile::segments);
            sFactor = VelocityProfile::s;
        } else if (act->state == Motion2State::ACCELERATING) {
            if (VelocityProfile::next()) {
                act->nextState();
            }
            sFactor = VelocityProfile::s;
        } else if (act->state == Motion2State::PLATEAU_INIT) {
            act->state = Motion2State::PLATEU;
            if (VelocityProfile::start(actM1->feedrate, actM1->feedrate, act->t2)) {
                act->nextState();
            }
            sFactor = VelocityProfile::s + act->s1;
        } else if (act->state == Motion2State::PLATEU) {
            if (VelocityProfile::next()) {
                act->nextState();
            }
            sFactor = VelocityProfile::s + act->s1;
        } else if (act->state == Motion2State::DECCELERATE_INIT) {
            act->state = Motion2State::DECELERATING;
            act->soff = act->s1 + act->s2;
            if (VelocityProfile::start(actM1->feedrate, actM1->endSpeed, act->t3)) {
                act->nextState();
            }
            sFactor = VelocityProfile::s + act->soff;
        } else if (act->state == Motion2State::DECELERATING) {
            if (VelocityProfile::next()) {
                act->nextState();
            }
            sFactor = VelocityProfile::s + act->soff;
        } else if (act->state == Motion2State::FINISHED) {
            m3->directions = 0;
            m3->usedAxes = 0;
            m3->stepsRemaining = 1;
            // Need to add this move to handle finished state correctly
            Motion3::pushReserved();
            return;
        }
        m3->last = Motion3::skipParentId == act->id;
        if (act->state == Motion2State::FINISHED) {
            // prevent rounding errors
            sFactor = actM1->length;
            m3->last = 1;
        } else {
            if (sFactor > actM1->length) {
                sFactor = actM1->length;
                m3->last = 1;
            }
        }
        // Com::printFLN("sf:", sFactor, 4);
        // Convert float position to integer motor position
        // This step catches all nonlinear behaviour from
        // acceleration profile and printer geometry
        float pos[NUM_AXES];
        fast8_t nextMotorIdx = 1 - lastMotorIdx;
        int32_t* np = lastMotorPos[nextMotorIdx];
        int32_t* lp = lastMotorPos[lastMotorIdx];
        FOR_ALL_AXES(i) {
            np[i] = lroundf(actM1->start[i] + sFactor * actM1->unitDir[i]);
        }
        // Fill structures used to update bresenham
        m3->directions = 0;
        m3->usedAxes = 0;
        if ((m3->stepsRemaining = VelocityProfile::stepsPerSegment) == 0) {
            if (m3->last) { // extreme case, normally never happens
                m3->usedAxes = 0;
                m3->stepsRemaining = 1;
                // Need to add this move to handle finished state correctly
                Motion3::pushReserved();
            }
            return; // don't add empty moves
        }
        m3->errorUpdate = (m3->stepsRemaining << 1);
        FOR_ALL_AXES(i) {
            if ((m3->delta[i] = ((np[i] - lp[i]) << 1)) < 0) {
                m3->delta[i] = -m3->delta[i];
                m3->usedAxes |= axisBits[i];
            } else if (m3->delta[i] != 0) {
                m3->directions |= axisBits[i];
                m3->usedAxes |= axisBits[i];
            }
            m3->error[i] = -(m3->stepsRemaining);
        }
        lastMotorIdx = nextMotorIdx;
        m3->parentId = act->id;
        m3->checkEndstops = actM1->isCheckEndstops();
        Tool* tool = Tool::getActiveTool();
        if (tool) {
            Motion1Buffer* motion1 = act->motion1;
            m3->secondSpeed = tool->computeIntensity(VelocityProfile::f, motion1->isActiveSecondary(), motion1->secondSpeed, motion1->secondSpeedPerMMPS);
        } else {
            m3->secondSpeed = 0;
        }
        PrinterType::enableMotors(m3->usedAxes);
        if (m3->last) {
            actM1 = nullptr; // select next on next interrupt
        }
    } else if (actM1->action == Motion1Action::WAIT || actM1->action == Motion1Action::WARMUP) { // just wait a bit
        m3->parentId = act->id;
        m3->usedAxes = 0;
        m3->checkEndstops = 0;
        m3->secondSpeed = actM1->secondSpeed;
        FOR_ALL_AXES(i) {
            m3->delta[i] = 0;
            m3->error[i] = 0;
        }
        if (actM1->feedrate > 32000) {
            m3->stepsRemaining = 32000;
            m3->last = 0;
            actM1->feedrate -= 32000;
        } else {
            m3->stepsRemaining = static_cast<unsigned int>(actM1->feedrate);
            m3->last = 1;
            actM1 = nullptr; // select next on next interrupt
        }
    } else {
        // Unknown action, skip it
        actM1 = nullptr;
    }
    Motion3::pushReserved();
}

// Gets called when an end stop is triggered during motion.
// Will stop all motions stored. For z probing and homing We
// Also note the remainig z steps.

void motorEndstopTriggered(fast8_t axis, bool dir) {
    Motion1::motorTriggered |= axisBits[axis];
    if (dir) {
        Motion1::motorDirTriggered |= axisBits[axis];
    } else {
        Motion1::motorDirTriggered &= ~axisBits[axis];
    }
}
void Motion2::motorEndstopTriggered(fast8_t axis, bool dir) {
    Motion1::motorTriggered |= axisBits[axis];
    if (dir) {
        Motion1::motorDirTriggered |= axisBits[axis];
    } else {
        Motion1::motorDirTriggered &= ~axisBits[axis];
    }
    Com::printFLN(PSTR("MotorTrigger:"), (int)Motion1::motorTriggered); // TEST
    /*Motion1::setAxisHomed(axis, false);
    Motion2Buffer& m2 = Motion2::buffers[act->parentId];
    if (Motion1::endstopMode == EndstopMode::STOP_AT_ANY_HIT || Motion1::endstopMode == EndstopMode::PROBING) {
        FOR_ALL_AXES(i) {
            Motion1::stepsRemaining[i] = m2.stepsRemaining[i];
        }
        Motion3::skipParentId = act->parentId;
        if (Motion1::endstopMode == EndstopMode::STOP_AT_ANY_HIT) {
            // TODO: Trigger fatal error
        }
    } else { // only mark hit axis
        Motion1::stepsRemaining[axis] = m2.stepsRemaining[axis];
        if ((Motion1::stopMask & Motion1::axesTriggered) == Motion1::stopMask) {
            Motion3::skipParentId = act->parentId;
        }
    }*/
}

void endstopTriggered(fast8_t axis, bool dir) {
    InterruptProtectedBlock noInt;
    Motion2::endstopTriggered(Motion3::act, axis, dir);
}

void Motion2::endstopTriggered(Motion3Buffer* act, fast8_t axis, bool dir) {
    // DEBUG_MSG2_FAST("EH:", (int)axis);
    if (act == nullptr || act->checkEndstops == false) {
        // DEBUG_MSG_FAST("EHX");
        return;
    }
    fast8_t bit = axisBits[axis];
    Motion1::axesTriggered = bit;
    if (dir) {
        Motion1::axesDirTriggered |= bit;
    } else {
        Motion1::axesDirTriggered &= ~bit;
    }
    Motion2Buffer& m2 = Motion2::buffers[act->parentId];
    Motion1Buffer* m1 = m2.motion1;
    if ((m1->axisUsed & bit) == 0) { // not motion directory
        return;
    }
    if ((m1->axisDir & bit) != (Motion1::axesDirTriggered & bit)) {
        return; // we move away so it is a stale signal from other direction
    }
    Motion1::setAxisHomed(axis, false);
    if (Motion1::endstopMode == EndstopMode::STOP_AT_ANY_HIT || Motion1::endstopMode == EndstopMode::PROBING) {
        FOR_ALL_AXES(i) {
            Motion1::stepsRemaining[i] = m2.stepsRemaining[i];
        }
        Motion3::skipParentId = act->parentId;
        if (Motion1::endstopMode == EndstopMode::STOP_AT_ANY_HIT) {
            // TODO: Trigger fatal error
        }
    } else { // only mark hit axis
        Motion1::stepsRemaining[axis] = m2.stepsRemaining[axis];
        if ((Motion1::stopMask & Motion1::axesTriggered) == Motion1::stopMask) {
            Motion3::skipParentId = act->parentId;
        }
    }
    // DEBUG_MSG_FAST("EHF");
}

void Motion2Buffer::nextState() {
    if (state == Motion2State::NOT_INITIALIZED) {
        if (t1 > 0) {
            state = Motion2State::ACCELERATE_INIT;
            return;
        }
        if (t2 > 0) {
            state = Motion2State::PLATEAU_INIT;
            return;
        }
        if (t3 > 0) {
            state = Motion2State::DECCELERATE_INIT;
            return;
        }
    }
    if (state == Motion2State::ACCELERATING) {
        if (t2 > 0) {
            state = Motion2State::PLATEAU_INIT;
            return;
        }
        if (t3 > 0) {
            state = Motion2State::DECCELERATE_INIT;
            return;
        }
    }
    if (state == Motion2State::PLATEU) {
        if (t3 > 0) {
            state = Motion2State::DECCELERATE_INIT;
            return;
        }
    }
    state = Motion2State::FINISHED;
}

void Motion2::copyMotorPos(int32_t pos[NUM_AXES]) {
    FOR_ALL_AXES(i) {
        pos[i] = lastMotorPos[lastMotorIdx][i];
    }
}

void Motion2::setMotorPositionFromTransformed() {
    PrinterType::transform(Motion1::currentPositionTransformed, lastMotorPos[lastMotorIdx]);
}

void Motion2::reportBuffers() {
    Com::printFLN(PSTR("M2 Buffer:"));
    Com::printFLN(PSTR("m1 ptr:"), (int)actM1);
    Com::printFLN(PSTR("length:"), (int)length);
    Com::printFLN(PSTR("nextActId:"), (int)nextActId);
}