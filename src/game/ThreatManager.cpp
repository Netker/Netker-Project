/*
 * Copyright (C) 2005-2009 MaNGOS <http://getmangos.com/>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include "ThreatManager.h"
#include "Unit.h"
#include "Creature.h"
#include "CreatureAI.h"
#include "Map.h"
#include "Player.h"
#include "ObjectAccessor.h"
#include "UnitEvents.h"

//==============================================================
//================= ThreatCalcHelper ===========================
//==============================================================

// The pHatingUnit is not used yet
float ThreatCalcHelper::calcThreat(Unit* pHatedUnit, Unit* pHatingUnit, float pThreat, bool crit, SpellSchoolMask schoolMask, SpellEntry const *pThreatSpell)
{
    // all flat mods applied early
    if(!pThreat)
        return 0.0f;

    if (pThreatSpell)
    {
        if (Player* modOwner = pHatedUnit->GetSpellModOwner())
            modOwner->ApplySpellMod(pThreatSpell->Id, SPELLMOD_THREAT, pThreat);

        if(crit)
            pThreat *= pHatedUnit->GetTotalAuraMultiplierByMiscMask(SPELL_AURA_MOD_CRITICAL_THREAT,schoolMask);
    }

    float threat = pHatedUnit->ApplyTotalThreatModifier(pThreat, schoolMask);
    return threat;
}

//============================================================
//================= HostileReference ==========================
//============================================================

HostileReference::HostileReference(Unit* pUnit, ThreatManager *pThreatManager, float pThreat)
{
    iThreat = pThreat;
    iTempThreatModifyer = 0.0f;
    link(pUnit, pThreatManager);
    iUnitGuid = pUnit->GetGUID();
    iOnline = true;
    iAccessible = true;
}

//============================================================
// Tell our refTo (target) object that we have a link
void HostileReference::targetObjectBuildLink()
{
    getTarget()->addHatedBy(this);
}

//============================================================
// Tell our refTo (taget) object, that the link is cut
void HostileReference::targetObjectDestroyLink()
{
    getTarget()->removeHatedBy(this);
}

//============================================================
// Tell our refFrom (source) object, that the link is cut (Target destroyed)

void HostileReference::sourceObjectDestroyLink()
{
    setOnlineOfflineState(false);
}

//============================================================
// Inform the source, that the status of the reference changed

void HostileReference::fireStatusChanged(ThreatRefStatusChangeEvent& pThreatRefStatusChangeEvent)
{
    if(getSource())
        getSource()->processThreatEvent(&pThreatRefStatusChangeEvent);
}

//============================================================

void HostileReference::addThreat(float pMod)
{
    iThreat += pMod;
    // the threat is changed. Source and target unit have to be availabe
    // if the link was cut before relink it again
    if(!isOnline())
        updateOnlineStatus();
    if(pMod != 0.0f)
    {
        ThreatRefStatusChangeEvent event(UEV_THREAT_REF_THREAT_CHANGE, this, pMod);
        fireStatusChanged(event);
    }

    if(isValid() && pMod >= 0)
    {
        Unit* victim_owner = getTarget()->GetOwner();
        if(victim_owner && victim_owner->isAlive())
            getSource()->addThreat(victim_owner, 0.0f);     // create a threat to the owner of a pet, if the pet attacks
    }
}

//============================================================
// check, if source can reach target and set the status

void HostileReference::updateOnlineStatus()
{
    bool online = false;
    bool accessible = false;

    if(!isValid())
    {
        Unit* target = ObjectAccessor::GetUnit(*getSourceUnit(), getUnitGuid());
        if(target)
            link(target, getSource());
    }
    // only check for online status if
    // ref is valid
    // target is no player or not gamemaster
    // target is not in flight
    if(isValid() &&
        ((getTarget()->GetTypeId() != TYPEID_PLAYER || !((Player*)getTarget())->isGameMaster()) ||
        !getTarget()->hasUnitState(UNIT_STAT_IN_FLIGHT)))
    {
        Creature* creature = (Creature* ) getSourceUnit();
        online = getTarget()->isInAccessablePlaceFor(creature);
        if(!online)
        {
            if(creature->AI()->canReachByRangeAttack(getTarget()))
                online = true;                              // not accessable but stays online
        }
        else
            accessible = true;

    }
    setAccessibleState(accessible);
    setOnlineOfflineState(online);
}

//============================================================
// set the status and fire the event on status change

void HostileReference::setOnlineOfflineState(bool pIsOnline)
{
    if(iOnline != pIsOnline)
    {
        iOnline = pIsOnline;
        if(!iOnline)
            setAccessibleState(false);                      // if not online that not accessable as well

        ThreatRefStatusChangeEvent event(UEV_THREAT_REF_ONLINE_STATUS, this);
        fireStatusChanged(event);
    }
}

//============================================================

void HostileReference::setAccessibleState(bool pIsAccessible)
{
    if(iAccessible != pIsAccessible)
    {
        iAccessible = pIsAccessible;

        ThreatRefStatusChangeEvent event(UEV_THREAT_REF_ASSECCIBLE_STATUS, this);
        fireStatusChanged(event);
    }
}

//============================================================
// prepare the reference for deleting
// this is called be the target

void HostileReference::removeReference()
{
    invalidate();

    ThreatRefStatusChangeEvent event(UEV_THREAT_REF_REMOVE_FROM_LIST, this);
    fireStatusChanged(event);
}

//============================================================

Unit* HostileReference::getSourceUnit()
{
    return (getSource()->getOwner());
}

//============================================================
//================ ThreatContainer ===========================
//============================================================

void ThreatContainer::clearReferences()
{
    for(std::list<HostileReference*>::const_iterator i = iThreatList.begin(); i != iThreatList.end(); ++i)
    {
        (*i)->unlink();
        delete (*i);
    }
    iThreatList.clear();
}

//============================================================
// Return the HostileReference of NULL, if not found
HostileReference* ThreatContainer::getReferenceByTarget(Unit* pVictim)
{
    HostileReference* result = NULL;

    uint64 guid = pVictim->GetGUID();
    for(std::list<HostileReference*>::const_iterator i = iThreatList.begin(); i != iThreatList.end(); ++i)
    {
        if((*i)->getUnitGuid() == guid)
        {
            result = (*i);
            break;
        }
    }

    return result;
}

//============================================================
// Add the threat, if we find the reference

HostileReference* ThreatContainer::addThreat(Unit* pVictim, float pThreat)
{
    HostileReference* ref = getReferenceByTarget(pVictim);
    if(ref)
        ref->addThreat(pThreat);
    return ref;
}

//============================================================

void ThreatContainer::modifyThreatPercent(Unit *pVictim, int32 pPercent)
{
    if(HostileReference* ref = getReferenceByTarget(pVictim))
        ref->addThreatPercent(pPercent);
}

//============================================================

bool HostileReferenceSortPredicate(const HostileReference* lhs, const HostileReference* rhs)
{
    // std::list::sort ordering predicate must be: (Pred(x,y)&&Pred(y,x))==false
    return lhs->getThreat() > rhs->getThreat();             // reverse sorting
}

//============================================================
// Check if the list is dirty and sort if necessary

void ThreatContainer::update()
{
    if(iDirty && iThreatList.size() >1)
    {
        iThreatList.sort(HostileReferenceSortPredicate);
    }
    iDirty = false;
}

//============================================================
// return the next best victim
// could be the current victim

HostileReference* ThreatContainer::selectNextVictim(Creature* pAttacker, HostileReference* pCurrentVictim)
{
    HostileReference* currentRef = NULL;
    bool found = false;
    bool noPriorityTargetFound = false;

    std::list<HostileReference*>::const_iterator lastRef = iThreatList.end();
    lastRef--;

    for(std::list<HostileReference*>::const_iterator iter = iThreatList.begin(); iter != iThreatList.end() && !found;)
    {
        currentRef = (*iter);

        Unit* target = currentRef->getTarget();
        assert(target);                                     // if the ref has status online the target must be there !

        // some units are prefered in comparison to others
        if(!noPriorityTargetFound && (target->IsImmunedToDamage(pAttacker->GetMeleeDamageSchoolMask()) || target->hasNegativeAuraWithInterruptFlag(AURA_INTERRUPT_FLAG_DAMAGE)) )
        {
            if(iter != lastRef)
            {
                // current victim is a second choice target, so don't compare threat with it below
                if(currentRef == pCurrentVictim)
                    pCurrentVictim = NULL;
                ++iter;
                continue;
            }
            else
            {
                // if we reached to this point, everyone in the threatlist is a second choice target. In such a situation the target with the highest threat should be attacked.
                noPriorityTargetFound = true;
                iter = iThreatList.begin();
                continue;
            }
        }

        if(!pAttacker->IsOutOfThreatArea(target))           // skip non attackable currently targets
        {
            if(pCurrentVictim)                              // select 1.3/1.1 better target in comparison current target
            {
                // list sorted and and we check current target, then this is best case
                if(pCurrentVictim == currentRef || currentRef->getThreat() <= 1.1f * pCurrentVictim->getThreat() )
                {
                    currentRef = pCurrentVictim;            // for second case
                    found = true;
                    break;
                }

                if (currentRef->getThreat() > 1.3f * pCurrentVictim->getThreat() ||
                     (currentRef->getThreat() > 1.1f * pCurrentVictim->getThreat() &&
                     pAttacker->IsWithinDistInMap(target, ATTACK_DISTANCE)) )
                {                                           //implement 110% threat rule for targets in melee range
                    found = true;                           //and 130% rule for targets in ranged distances
                    break;                                  //for selecting alive targets
                }
            }
            else                                            // select any
            {
                found = true;
                break;
            }
        }
        ++iter;
    }
    if(!found)
        currentRef = NULL;

    return currentRef;
}

//============================================================
//=================== ThreatManager ==========================
//============================================================

ThreatManager::ThreatManager(Unit* owner) : iCurrentVictim(NULL), iOwner(owner)
{
}

//============================================================

void ThreatManager::clearReferences()
{
    iThreatContainer.clearReferences();
    iThreatOfflineContainer.clearReferences();
    iCurrentVictim = NULL;
}

//============================================================

void ThreatManager::addThreat(Unit* pVictim, float pThreat, bool crit, SpellSchoolMask schoolMask, SpellEntry const *pThreatSpell)
{
    //function deals with adding threat and adding players and pets into ThreatList
    //mobs, NPCs, guards have ThreatList and HateOfflineList
    //players and pets have only InHateListOf
    //HateOfflineList is used co contain unattackable victims (in-flight, in-water, GM etc.)

    // not to self
    if (pVictim == getOwner())
        return;

    // not to GM
    if(!pVictim || (pVictim->GetTypeId() == TYPEID_PLAYER && ((Player*)pVictim)->isGameMaster()) )
        return;

    // not to dead and not for dead
    if(!pVictim->isAlive() || !getOwner()->isAlive() )
        return;

    assert(getOwner()->GetTypeId()== TYPEID_UNIT);

    float threat = ThreatCalcHelper::calcThreat(pVictim, iOwner, pThreat, crit, schoolMask, pThreatSpell);

    HostileReference* ref = iThreatContainer.addThreat(pVictim, threat);
    // Ref is not in the online refs, search the offline refs next
    if(!ref)
        ref = iThreatOfflineContainer.addThreat(pVictim, threat);

    if(!ref)                                                // there was no ref => create a new one
    {
                                                            // threat has to be 0 here
        HostileReference* hostileReference = new HostileReference(pVictim, this, 0);
        iThreatContainer.addReference(hostileReference);
        hostileReference->addThreat(threat);                // now we add the real threat
        if(pVictim->GetTypeId() == TYPEID_PLAYER && ((Player*)pVictim)->isGameMaster())
            hostileReference->setOnlineOfflineState(false); // GM is always offline
    }
}

//============================================================

void ThreatManager::modifyThreatPercent(Unit *pVictim, int32 pPercent)
{
    iThreatContainer.modifyThreatPercent(pVictim, pPercent);
}

//============================================================

Unit* ThreatManager::getHostileTarget()
{
    iThreatContainer.update();
    HostileReference* nextVictim = iThreatContainer.selectNextVictim((Creature*) getOwner(), getCurrentVictim());
    setCurrentVictim(nextVictim);
    return getCurrentVictim() != NULL ? getCurrentVictim()->getTarget() : NULL;
}

//============================================================

float ThreatManager::getThreat(Unit *pVictim, bool pAlsoSearchOfflineList)
{
    float threat = 0.0f;
    HostileReference* ref = iThreatContainer.getReferenceByTarget(pVictim);
    if(!ref && pAlsoSearchOfflineList)
        ref = iThreatOfflineContainer.getReferenceByTarget(pVictim);
    if(ref)
        threat = ref->getThreat();
    return threat;
}

//============================================================

void ThreatManager::tauntApply(Unit* pTaunter)
{
    HostileReference* ref = iThreatContainer.getReferenceByTarget(pTaunter);
    if(getCurrentVictim() && ref && (ref->getThreat() < getCurrentVictim()->getThreat()))
    {
        if(ref->getTempThreatModifyer() == 0.0f)
                                                            // Ok, temp threat is unused
            ref->setTempThreat(getCurrentVictim()->getThreat());
    }
}

//============================================================

void ThreatManager::tauntFadeOut(Unit *pTaunter)
{
    HostileReference* ref = iThreatContainer.getReferenceByTarget(pTaunter);
    if(ref)
        ref->resetTempThreat();
}

//============================================================

void ThreatManager::setCurrentVictim(HostileReference* pHostileReference)
{
     iCurrentVictim = pHostileReference;
}

//============================================================
// The hated unit is gone, dead or deleted
// return true, if the event is consumed

void ThreatManager::processThreatEvent(ThreatRefStatusChangeEvent* threatRefStatusChangeEvent)
{
    threatRefStatusChangeEvent->setThreatManager(this);     // now we can set the threat manager

    HostileReference* hostileReference = threatRefStatusChangeEvent->getReference();

    switch(threatRefStatusChangeEvent->getType())
    {
        case UEV_THREAT_REF_THREAT_CHANGE:
            if((getCurrentVictim() == hostileReference && threatRefStatusChangeEvent->getFValue()<0.0f) ||
                (getCurrentVictim() != hostileReference && threatRefStatusChangeEvent->getFValue()>0.0f))
                setDirty(true);                             // the order in the threat list might have changed
            break;
        case UEV_THREAT_REF_ONLINE_STATUS:
            if(!hostileReference->isOnline())
            {
                if (hostileReference == getCurrentVictim())
                {
                    setCurrentVictim(NULL);
                    setDirty(true);
                }
                iThreatContainer.remove(hostileReference);
                iThreatOfflineContainer.addReference(hostileReference);
            }
            else
            {
                if(getCurrentVictim() && hostileReference->getThreat() > (1.1f * getCurrentVictim()->getThreat()))
                    setDirty(true);
                iThreatContainer.addReference(hostileReference);
                iThreatOfflineContainer.remove(hostileReference);
            }
            break;
        case UEV_THREAT_REF_REMOVE_FROM_LIST:
            if (hostileReference == getCurrentVictim())
            {
                setCurrentVictim(NULL);
                setDirty(true);
            }
            if(hostileReference->isOnline())
                iThreatContainer.remove(hostileReference);
            else
                iThreatOfflineContainer.remove(hostileReference);
            break;
    }
}