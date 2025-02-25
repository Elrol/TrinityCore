/*
 * This file is part of the TrinityCore Project. See AUTHORS file for Copyright information
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include "Object.h"
#include "AreaTriggerPackets.h"
#include "AreaTriggerTemplate.h"
#include "BattlefieldMgr.h"
#include "CellImpl.h"
#include "CinematicMgr.h"
#include "Common.h"
#include "Creature.h"
#include "GameTime.h"
#include "GridNotifiersImpl.h"
#include "InstanceScenario.h"
#include "Item.h"
#include "Log.h"
#include "MiscPackets.h"
#include "MovementPackets.h"
#include "MovementTypedefs.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "OutdoorPvPMgr.h"
#include "PhasingHandler.h"
#include "Player.h"
#include "SharedDefines.h"
#include "SpellAuraEffects.h"
#include "TemporarySummon.h"
#include "Totem.h"
#include "Transport.h"
#include "Unit.h"
#include "UpdateData.h"
#include "Util.h"
#include "VMapFactory.h"
#include "Vehicle.h"
#include "World.h"
#include "WorldSession.h"
#include <G3D/Vector3.h>

constexpr float VisibilityDistances[AsUnderlyingType(VisibilityDistanceType::Max)] =
{
    DEFAULT_VISIBILITY_DISTANCE,
    VISIBILITY_DISTANCE_TINY,
    VISIBILITY_DISTANCE_SMALL,
    VISIBILITY_DISTANCE_LARGE,
    VISIBILITY_DISTANCE_GIGANTIC,
    MAX_VISIBILITY_DISTANCE
};

Object::Object() : m_values(this)
{
    m_objectTypeId      = TYPEID_OBJECT;
    m_objectType        = TYPEMASK_OBJECT;
    m_updateFlag.Clear();

    m_inWorld           = false;
    m_isNewObject       = false;
    m_objectUpdated     = false;
}

WorldObject::~WorldObject()
{
    // this may happen because there are many !create/delete
    if (IsWorldObject() && m_currMap)
    {
        if (GetTypeId() == TYPEID_CORPSE)
        {
            TC_LOG_FATAL("misc", "WorldObject::~WorldObject Corpse Type: %d (%s) deleted but still in map!!",
                ToCorpse()->GetType(), GetGUID().ToString().c_str());
            ABORT();
        }
        ResetMap();
    }
}

Object::~Object()
{
    if (IsInWorld())
    {
        TC_LOG_FATAL("misc", "Object::~Object %s deleted but still in world!!", GetGUID().ToString().c_str());
        if (isType(TYPEMASK_ITEM))
            TC_LOG_FATAL("misc", "Item slot %u", ((Item*)this)->GetSlot());
        ABORT();
    }

    if (m_objectUpdated)
    {
        TC_LOG_FATAL("misc", "Object::~Object %s deleted but still in update list!!", GetGUID().ToString().c_str());
        ABORT();
    }
}

void Object::_Create(ObjectGuid const& guid)
{
    m_objectUpdated = false;
    m_guid = guid;
}

void Object::AddToWorld()
{
    if (m_inWorld)
        return;

    m_inWorld = true;

    // synchronize values mirror with values array (changes will send in updatecreate opcode any way
    ASSERT(!m_objectUpdated);
    ClearUpdateMask(false);
}

void Object::RemoveFromWorld()
{
    if (!m_inWorld)
        return;

    m_inWorld = false;

    // if we remove from world then sending changes not required
    ClearUpdateMask(true);
}

void Object::BuildCreateUpdateBlockForPlayer(UpdateData* data, Player* target) const
{
    if (!target)
        return;

    uint8 updateType = UPDATETYPE_CREATE_OBJECT;
    uint8 objectType = m_objectTypeId;
    uint16 objectTypeMask = m_objectType;
    CreateObjectBits flags = m_updateFlag;

    /** lower flag1 **/
    if (target == this)                                      // building packet for yourself
    {
        flags.ThisIsYou = true;
        flags.ActivePlayer = true;
        objectType = TYPEID_ACTIVE_PLAYER;
        objectTypeMask |= TYPEMASK_ACTIVE_PLAYER;
    }

    switch (GetGUID().GetHigh())
    {
        case HighGuid::Player:
        case HighGuid::Pet:
        case HighGuid::Corpse:
        case HighGuid::DynamicObject:
        case HighGuid::AreaTrigger:
        case HighGuid::Conversation:
            updateType = UPDATETYPE_CREATE_OBJECT2;
            break;
        case HighGuid::Creature:
        case HighGuid::Vehicle:
        {
            if (TempSummon const* summon = ToUnit()->ToTempSummon())
                if (summon->GetSummonerGUID().IsPlayer())
                    updateType = UPDATETYPE_CREATE_OBJECT2;

            break;
        }
        case HighGuid::GameObject:
        {
            if (ToGameObject()->GetOwnerGUID().IsPlayer())
                updateType = UPDATETYPE_CREATE_OBJECT2;
            break;
        }
        default:
            break;
    }

    if (WorldObject const* worldObject = dynamic_cast<WorldObject const*>(this))
    {
        if (!flags.MovementUpdate && !worldObject->m_movementInfo.transport.guid.IsEmpty())
            flags.MovementTransport = true;

        if (worldObject->GetAIAnimKitId() || worldObject->GetMovementAnimKitId() || worldObject->GetMeleeAnimKitId())
            flags.AnimKit = true;
    }

    if (flags.Stationary)
    {
        // UPDATETYPE_CREATE_OBJECT2 for some gameobject types...
        if (isType(TYPEMASK_GAMEOBJECT))
        {
            switch (ToGameObject()->GetGoType())
            {
                case GAMEOBJECT_TYPE_TRAP:
                case GAMEOBJECT_TYPE_DUEL_ARBITER:
                case GAMEOBJECT_TYPE_FLAGSTAND:
                case GAMEOBJECT_TYPE_FLAGDROP:
                    updateType = UPDATETYPE_CREATE_OBJECT2;
                    break;
                default:
                    break;
            }
        }
    }

    if (Unit const* unit = ToUnit())
        if (unit->GetVictim())
            flags.CombatVictim = true;

    ByteBuffer buf(0x400, ByteBuffer::Reserve{});
    buf << uint8(updateType);
    buf << GetGUID();
    buf << uint8(objectType);

    BuildMovementUpdate(&buf, flags);
    BuildValuesCreate(&buf, target);
    data->AddUpdateBlock(buf);
}

void Object::SendUpdateToPlayer(Player* player)
{
    // send create update to player
    UpdateData upd(player->GetMapId());
    WorldPacket packet;

    if (player->HaveAtClient(this))
        BuildValuesUpdateBlockForPlayer(&upd, player);
    else
        BuildCreateUpdateBlockForPlayer(&upd, player);
    upd.BuildPacket(&packet);
    player->SendDirectMessage(&packet);
}

void Object::BuildValuesUpdateBlockForPlayer(UpdateData* data, Player const* target) const
{
    ByteBuffer buf = PrepareValuesUpdateBuffer();

    BuildValuesUpdate(&buf, target);

    data->AddUpdateBlock(buf);
}

void Object::BuildValuesUpdateBlockForPlayerWithFlag(UpdateData* data, UF::UpdateFieldFlag flags, Player const* target) const
{
    ByteBuffer buf = PrepareValuesUpdateBuffer();

    BuildValuesUpdateWithFlag(&buf, flags, target);

    data->AddUpdateBlock(buf);
}

void Object::BuildDestroyUpdateBlock(UpdateData* data) const
{
    data->AddDestroyObject(GetGUID());
}

void Object::BuildOutOfRangeUpdateBlock(UpdateData* data) const
{
    data->AddOutOfRangeGUID(GetGUID());
}

ByteBuffer Object::PrepareValuesUpdateBuffer() const
{
    ByteBuffer buffer(500, ByteBuffer::Reserve{});
    buffer << uint8(UPDATETYPE_VALUES);
    buffer << GetGUID();
    return buffer;
}

void Object::DestroyForPlayer(Player* target) const
{
    ASSERT(target);

    UpdateData updateData(target->GetMapId());
    BuildDestroyUpdateBlock(&updateData);
    WorldPacket packet;
    updateData.BuildPacket(&packet);
    target->SendDirectMessage(&packet);
}

void Object::BuildMovementUpdate(ByteBuffer* data, CreateObjectBits flags) const
{
    std::vector<uint32> const* PauseTimes = nullptr;
    uint32 PauseTimesCount = 0;
    if (GameObject const* go = ToGameObject())
    {
        if (go->GetGoType() == GAMEOBJECT_TYPE_TRANSPORT)
        {
            PauseTimes = go->GetGOValue()->Transport.StopFrames;
            PauseTimesCount = PauseTimes->size();
        }
    }

    data->WriteBit(flags.NoBirthAnim);
    data->WriteBit(flags.EnablePortals);
    data->WriteBit(flags.PlayHoverAnim);
    data->WriteBit(flags.MovementUpdate);
    data->WriteBit(flags.MovementTransport);
    data->WriteBit(flags.Stationary);
    data->WriteBit(flags.CombatVictim);
    data->WriteBit(flags.ServerTime);
    data->WriteBit(flags.Vehicle);
    data->WriteBit(flags.AnimKit);
    data->WriteBit(flags.Rotation);
    data->WriteBit(flags.AreaTrigger);
    data->WriteBit(flags.GameObject);
    data->WriteBit(flags.SmoothPhasing);
    data->WriteBit(flags.ThisIsYou);
    data->WriteBit(flags.SceneObject);
    data->WriteBit(flags.ActivePlayer);
    data->WriteBit(flags.Conversation);
    data->FlushBits();

    if (flags.MovementUpdate)
    {
        Unit const* unit = ToUnit();
        bool HasFallDirection = unit->HasUnitMovementFlag(MOVEMENTFLAG_FALLING);
        bool HasFall = HasFallDirection || unit->m_movementInfo.jump.fallTime != 0;
        bool HasSpline = unit->IsSplineEnabled();

        *data << GetGUID();                                             // MoverGUID

        *data << uint32(unit->m_movementInfo.time);                     // MoveTime
        *data << float(unit->GetPositionX());
        *data << float(unit->GetPositionY());
        *data << float(unit->GetPositionZ());
        *data << float(unit->GetOrientation());

        *data << float(unit->m_movementInfo.pitch);                     // Pitch
        *data << float(unit->m_movementInfo.splineElevation);           // StepUpStartElevation

        *data << uint32(0);                                             // RemoveForcesIDs.size()
        *data << uint32(0);                                             // MoveIndex

        //for (std::size_t i = 0; i < RemoveForcesIDs.size(); ++i)
        //    *data << ObjectGuid(RemoveForcesIDs);

        data->WriteBits(unit->GetUnitMovementFlags(), 30);
        data->WriteBits(unit->GetExtraUnitMovementFlags(), 18);
        data->WriteBit(!unit->m_movementInfo.transport.guid.IsEmpty()); // HasTransport
        data->WriteBit(HasFall);                                        // HasFall
        data->WriteBit(HasSpline);                                      // HasSpline - marks that the unit uses spline movement
        data->WriteBit(false);                                          // HeightChangeFailed
        data->WriteBit(false);                                          // RemoteTimeValid

        if (!unit->m_movementInfo.transport.guid.IsEmpty())
            *data << unit->m_movementInfo.transport;

        if (HasFall)
        {
            *data << uint32(unit->m_movementInfo.jump.fallTime);        // Time
            *data << float(unit->m_movementInfo.jump.zspeed);           // JumpVelocity

            if (data->WriteBit(HasFallDirection))
            {
                *data << float(unit->m_movementInfo.jump.sinAngle);     // Direction
                *data << float(unit->m_movementInfo.jump.cosAngle);
                *data << float(unit->m_movementInfo.jump.xyspeed);      // Speed
            }
        }

        *data << float(unit->GetSpeed(MOVE_WALK));
        *data << float(unit->GetSpeed(MOVE_RUN));
        *data << float(unit->GetSpeed(MOVE_RUN_BACK));
        *data << float(unit->GetSpeed(MOVE_SWIM));
        *data << float(unit->GetSpeed(MOVE_SWIM_BACK));
        *data << float(unit->GetSpeed(MOVE_FLIGHT));
        *data << float(unit->GetSpeed(MOVE_FLIGHT_BACK));
        *data << float(unit->GetSpeed(MOVE_TURN_RATE));
        *data << float(unit->GetSpeed(MOVE_PITCH_RATE));

        if (MovementForces const* movementForces = unit->GetMovementForces())
        {
            *data << uint32(movementForces->GetForces()->size());
            *data << float(movementForces->GetModMagnitude());          // MovementForcesModMagnitude
        }
        else
        {
            *data << uint32(0);
            *data << float(1.0f);                                       // MovementForcesModMagnitude
        }

        data->WriteBit(HasSpline);
        data->FlushBits();

        if (MovementForces const* movementForces = unit->GetMovementForces())
            for (MovementForce const& force : *movementForces->GetForces())
                WorldPackets::Movement::CommonMovement::WriteMovementForceWithDirection(force, *data, unit);

        if (HasSpline)
            WorldPackets::Movement::CommonMovement::WriteCreateObjectSplineDataBlock(*unit->movespline, *data);
    }

    *data << uint32(PauseTimesCount);

    if (flags.Stationary)
    {
        WorldObject const* self = static_cast<WorldObject const*>(this);
        *data << float(self->GetStationaryX());
        *data << float(self->GetStationaryY());
        *data << float(self->GetStationaryZ());
        *data << float(self->GetStationaryO());
    }

    if (flags.CombatVictim)
        *data << ToUnit()->GetVictim()->GetGUID();                      // CombatVictim

    if (flags.ServerTime)
    {
        GameObject const* go = ToGameObject();
        /** @TODO Use IsTransport() to also handle type 11 (TRANSPORT)
            Currently grid objects are not updated if there are no nearby players,
            this causes clients to receive different PathProgress
            resulting in players seeing the object in a different position
        */
        if (go && go->ToTransport())                                    // ServerTime
            *data << uint32(go->GetGOValue()->Transport.PathProgress);
        else
            *data << uint32(GameTime::GetGameTimeMS());
    }

    if (flags.Vehicle)
    {
        Unit const* unit = ToUnit();
        *data << uint32(unit->GetVehicleKit()->GetVehicleInfo()->ID); // RecID
        *data << float(unit->GetOrientation());                         // InitialRawFacing
    }

    if (flags.AnimKit)
    {
        WorldObject const* self = static_cast<WorldObject const*>(this);
        *data << uint16(self->GetAIAnimKitId());                        // AiID
        *data << uint16(self->GetMovementAnimKitId());                  // MovementID
        *data << uint16(self->GetMeleeAnimKitId());                     // MeleeID
    }

    if (flags.Rotation)
        *data << uint64(ToGameObject()->GetPackedWorldRotation());      // Rotation

    if (PauseTimesCount)
        data->append(PauseTimes->data(), PauseTimes->size());

    if (flags.MovementTransport)
    {
        WorldObject const* self = static_cast<WorldObject const*>(this);
        *data << self->m_movementInfo.transport;
    }

    if (flags.AreaTrigger)
    {
        AreaTrigger const* areaTrigger = ToAreaTrigger();
        AreaTriggerMiscTemplate const* areaTriggerMiscTemplate = areaTrigger->GetMiscTemplate();
        AreaTriggerTemplate const* areaTriggerTemplate = areaTrigger->GetTemplate();

        *data << uint32(areaTrigger->GetTimeSinceCreated());

        *data << areaTrigger->GetRollPitchYaw().PositionXYZStream();

        bool hasAbsoluteOrientation = areaTriggerTemplate->HasFlag(AREATRIGGER_FLAG_HAS_ABSOLUTE_ORIENTATION);
        bool hasDynamicShape        = areaTriggerTemplate->HasFlag(AREATRIGGER_FLAG_HAS_DYNAMIC_SHAPE);
        bool hasAttached            = areaTriggerTemplate->HasFlag(AREATRIGGER_FLAG_HAS_ATTACHED);
        bool hasFaceMovementDir     = areaTriggerTemplate->HasFlag(AREATRIGGER_FLAG_HAS_FACE_MOVEMENT_DIR);
        bool hasFollowsTerrain      = areaTriggerTemplate->HasFlag(AREATRIGGER_FLAG_HAS_FOLLOWS_TERRAIN);
        bool hasUnk1                = areaTriggerTemplate->HasFlag(AREATRIGGER_FLAG_UNK1);
        bool hasTargetRollPitchYaw  = areaTriggerTemplate->HasFlag(AREATRIGGER_FLAG_HAS_TARGET_ROLL_PITCH_YAW);
        bool hasScaleCurveID        = areaTriggerMiscTemplate->ScaleCurveId != 0;
        bool hasMorphCurveID        = areaTriggerMiscTemplate->MorphCurveId != 0;
        bool hasFacingCurveID       = areaTriggerMiscTemplate->FacingCurveId != 0;
        bool hasMoveCurveID         = areaTriggerMiscTemplate->MoveCurveId != 0;
        bool hasAnimation           = areaTriggerTemplate->HasFlag(AREATRIGGER_FLAG_HAS_ANIM_ID);
        bool hasUnk3                = areaTriggerTemplate->HasFlag(AREATRIGGER_FLAG_UNK3);
        bool hasAnimKitID           = areaTriggerTemplate->HasFlag(AREATRIGGER_FLAG_HAS_ANIM_KIT_ID);
        bool hasAnimProgress        = false;
        bool hasAreaTriggerSphere   = areaTriggerTemplate->IsSphere();
        bool hasAreaTriggerBox      = areaTriggerTemplate->IsBox();
        bool hasAreaTriggerPolygon  = areaTriggerTemplate->IsPolygon();
        bool hasAreaTriggerCylinder = areaTriggerTemplate->IsCylinder();
        bool hasAreaTriggerSpline   = areaTrigger->HasSplines();
        bool hasOrbit               = areaTrigger->HasOrbit();
        bool hasMovementScript      = false;

        data->WriteBit(hasAbsoluteOrientation);
        data->WriteBit(hasDynamicShape);
        data->WriteBit(hasAttached);
        data->WriteBit(hasFaceMovementDir);
        data->WriteBit(hasFollowsTerrain);
        data->WriteBit(hasUnk1);
        data->WriteBit(hasTargetRollPitchYaw);
        data->WriteBit(hasScaleCurveID);
        data->WriteBit(hasMorphCurveID);
        data->WriteBit(hasFacingCurveID);
        data->WriteBit(hasMoveCurveID);
        data->WriteBit(hasAnimation);
        data->WriteBit(hasAnimKitID);
        data->WriteBit(hasUnk3);
        data->WriteBit(hasAnimProgress);
        data->WriteBit(hasAreaTriggerSphere);
        data->WriteBit(hasAreaTriggerBox);
        data->WriteBit(hasAreaTriggerPolygon);
        data->WriteBit(hasAreaTriggerCylinder);
        data->WriteBit(hasAreaTriggerSpline);
        data->WriteBit(hasOrbit);
        data->WriteBit(hasMovementScript);

        if (hasUnk3)
            data->WriteBit(false);

        data->FlushBits();

        if (hasAreaTriggerSpline)
        {
            *data << uint32(areaTrigger->GetTimeToTarget());
            *data << uint32(areaTrigger->GetElapsedTimeForMovement());

            WorldPackets::Movement::CommonMovement::WriteCreateObjectAreaTriggerSpline(areaTrigger->GetSpline(), *data);
        }

        if (hasTargetRollPitchYaw)
            *data << areaTrigger->GetTargetRollPitchYaw().PositionXYZStream();

        if (hasScaleCurveID)
            *data << uint32(areaTriggerMiscTemplate->ScaleCurveId);

        if (hasMorphCurveID)
            *data << uint32(areaTriggerMiscTemplate->MorphCurveId);

        if (hasFacingCurveID)
            *data << uint32(areaTriggerMiscTemplate->FacingCurveId);

        if (hasMoveCurveID)
            *data << uint32(areaTriggerMiscTemplate->MoveCurveId);

        if (hasAnimation)
            *data << int32(areaTriggerMiscTemplate->AnimId);

        if (hasAnimKitID)
            *data << int32(areaTriggerMiscTemplate->AnimKitId);

        if (hasAnimProgress)
            *data << uint32(0);

        if (hasAreaTriggerSphere)
        {
            *data << float(areaTriggerTemplate->SphereDatas.Radius);
            *data << float(areaTriggerTemplate->SphereDatas.RadiusTarget);
        }

        if (hasAreaTriggerBox)
        {
            *data << float(areaTriggerTemplate->BoxDatas.Extents[0]);
            *data << float(areaTriggerTemplate->BoxDatas.Extents[1]);
            *data << float(areaTriggerTemplate->BoxDatas.Extents[2]);
            *data << float(areaTriggerTemplate->BoxDatas.ExtentsTarget[0]);
            *data << float(areaTriggerTemplate->BoxDatas.ExtentsTarget[1]);
            *data << float(areaTriggerTemplate->BoxDatas.ExtentsTarget[2]);
        }

        if (hasAreaTriggerPolygon)
        {
            *data << int32(areaTriggerTemplate->PolygonVertices.size());
            *data << int32(areaTriggerTemplate->PolygonVerticesTarget.size());
            *data << float(areaTriggerTemplate->PolygonDatas.Height);
            *data << float(areaTriggerTemplate->PolygonDatas.HeightTarget);

            for (TaggedPosition<Position::XY> const& vertice : areaTriggerTemplate->PolygonVertices)
                *data << vertice;

            for (TaggedPosition<Position::XY> const& vertice : areaTriggerTemplate->PolygonVerticesTarget)
                *data << vertice;
        }

        if (hasAreaTriggerCylinder)
        {
            *data << float(areaTriggerTemplate->CylinderDatas.Radius);
            *data << float(areaTriggerTemplate->CylinderDatas.RadiusTarget);
            *data << float(areaTriggerTemplate->CylinderDatas.Height);
            *data << float(areaTriggerTemplate->CylinderDatas.HeightTarget);
            *data << float(areaTriggerTemplate->CylinderDatas.LocationZOffset);
            *data << float(areaTriggerTemplate->CylinderDatas.LocationZOffsetTarget);
        }

        //if (hasMovementScript)
        //    *data << *areaTrigger->GetMovementScript(); // AreaTriggerMovementScriptInfo

        if (hasOrbit)
            *data << *areaTrigger->GetCircularMovementInfo();
    }

    if (flags.GameObject)
    {
        bool bit8 = false;
        uint32 Int1 = 0;

        GameObject const* gameObject = ToGameObject();

        *data << uint32(gameObject->GetWorldEffectID());

        data->WriteBit(bit8);
        data->FlushBits();
        if (bit8)
            *data << uint32(Int1);
    }

    //if (flags.SmoothPhasing)
    //{
    //    data->WriteBit(ReplaceActive);
    //    data->WriteBit(HasReplaceObject);
    //    data->FlushBits();
    //    if (HasReplaceObject)
    //        *data << ObjectGuid(ReplaceObject);
    //}

    //if (flags.SceneObject)
    //{
    //    data->WriteBit(HasLocalScriptData);
    //    data->WriteBit(HasPetBattleFullUpdate);
    //    data->FlushBits();

    //    if (HasLocalScriptData)
    //    {
    //        data->WriteBits(Data.length(), 7);
    //        data->FlushBits();
    //        data->WriteString(Data);
    //    }

    //    if (HasPetBattleFullUpdate)
    //    {
    //        for (std::size_t i = 0; i < 2; ++i)
    //        {
    //            *data << ObjectGuid(Players[i].CharacterID);
    //            *data << int32(Players[i].TrapAbilityID);
    //            *data << int32(Players[i].TrapStatus);
    //            *data << uint16(Players[i].RoundTimeSecs);
    //            *data << int8(Players[i].FrontPet);
    //            *data << uint8(Players[i].InputFlags);

    //            data->WriteBits(Players[i].Pets.size(), 2);
    //            data->FlushBits();
    //            for (std::size_t j = 0; j < Players[i].Pets.size(); ++j)
    //            {
    //                *data << ObjectGuid(Players[i].Pets[j].BattlePetGUID);
    //                *data << int32(Players[i].Pets[j].SpeciesID);
    //                *data << int32(Players[i].Pets[j].DisplayID);
    //                *data << int32(Players[i].Pets[j].CollarID);
    //                *data << int16(Players[i].Pets[j].Level);
    //                *data << int16(Players[i].Pets[j].Xp);
    //                *data << int32(Players[i].Pets[j].CurHealth);
    //                *data << int32(Players[i].Pets[j].MaxHealth);
    //                *data << int32(Players[i].Pets[j].Power);
    //                *data << int32(Players[i].Pets[j].Speed);
    //                *data << int32(Players[i].Pets[j].NpcTeamMemberID);
    //                *data << uint16(Players[i].Pets[j].BreedQuality);
    //                *data << uint16(Players[i].Pets[j].StatusFlags);
    //                *data << int8(Players[i].Pets[j].Slot);

    //                *data << uint32(Players[i].Pets[j].Abilities.size());
    //                *data << uint32(Players[i].Pets[j].Auras.size());
    //                *data << uint32(Players[i].Pets[j].States.size());
    //                for (std::size_t k = 0; k < Players[i].Pets[j].Abilities.size(); ++k)
    //                {
    //                    *data << int32(Players[i].Pets[j].Abilities[k].AbilityID);
    //                    *data << int16(Players[i].Pets[j].Abilities[k].CooldownRemaining);
    //                    *data << int16(Players[i].Pets[j].Abilities[k].LockdownRemaining);
    //                    *data << int8(Players[i].Pets[j].Abilities[k].AbilityIndex);
    //                    *data << uint8(Players[i].Pets[j].Abilities[k].Pboid);
    //                }

    //                for (std::size_t k = 0; k < Players[i].Pets[j].Auras.size(); ++k)
    //                {
    //                    *data << int32(Players[i].Pets[j].Auras[k].AbilityID);
    //                    *data << uint32(Players[i].Pets[j].Auras[k].InstanceID);
    //                    *data << int32(Players[i].Pets[j].Auras[k].RoundsRemaining);
    //                    *data << int32(Players[i].Pets[j].Auras[k].CurrentRound);
    //                    *data << uint8(Players[i].Pets[j].Auras[k].CasterPBOID);
    //                }

    //                for (std::size_t k = 0; k < Players[i].Pets[j].States.size(); ++k)
    //                {
    //                    *data << uint32(Players[i].Pets[j].States[k].StateID);
    //                    *data << int32(Players[i].Pets[j].States[k].StateValue);
    //                }

    //                data->WriteBits(Players[i].Pets[j].CustomName.length(), 7);
    //                data->FlushBits();
    //                data->WriteString(Players[i].Pets[j].CustomName);
    //            }
    //        }

    //        for (std::size_t i = 0; i < 3; ++i)
    //        {
    //            *data << uint32(Enviros[j].Auras.size());
    //            *data << uint32(Enviros[j].States.size());
    //            for (std::size_t j = 0; j < Enviros[j].Auras.size(); ++j)
    //            {
    //                *data << int32(Enviros[j].Auras[j].AbilityID);
    //                *data << uint32(Enviros[j].Auras[j].InstanceID);
    //                *data << int32(Enviros[j].Auras[j].RoundsRemaining);
    //                *data << int32(Enviros[j].Auras[j].CurrentRound);
    //                *data << uint8(Enviros[j].Auras[j].CasterPBOID);
    //            }

    //            for (std::size_t j = 0; j < Enviros[j].States.size(); ++j)
    //            {
    //                *data << uint32(Enviros[i].States[j].StateID);
    //                *data << int32(Enviros[i].States[j].StateValue);
    //            }
    //        }

    //        *data << uint16(WaitingForFrontPetsMaxSecs);
    //        *data << uint16(PvpMaxRoundTime);
    //        *data << int32(CurRound);
    //        *data << uint32(NpcCreatureID);
    //        *data << uint32(NpcDisplayID);
    //        *data << int8(CurPetBattleState);
    //        *data << uint8(ForfeitPenalty);
    //        *data << ObjectGuid(InitialWildPetGUID);
    //        data->WriteBit(IsPVP);
    //        data->WriteBit(CanAwardXP);
    //        data->FlushBits();
    //    }
    //}

    if (flags.ActivePlayer)
    {
        bool HasSceneInstanceIDs = false;
        bool HasRuneState = ToUnit()->GetPowerIndex(POWER_RUNES) != MAX_POWERS;

        data->WriteBit(HasSceneInstanceIDs);
        data->WriteBit(HasRuneState);
        data->FlushBits();
        //if (HasSceneInstanceIDs)
        //{
        //    *data << uint32(SceneInstanceIDs.size());
        //    for (std::size_t i = 0; i < SceneInstanceIDs.size(); ++i)
        //        *data << uint32(SceneInstanceIDs[i]);
        //}
        if (HasRuneState)
        {
            Player const* player = ToPlayer();
            float baseCd = float(player->GetRuneBaseCooldown());
            uint32 maxRunes = uint32(player->GetMaxPower(POWER_RUNES));

            *data << uint8((1 << maxRunes) - 1);
            *data << uint8(player->GetRunesState());
            *data << uint32(maxRunes);
            for (uint32 i = 0; i < maxRunes; ++i)
                *data << uint8((baseCd - float(player->GetRuneCooldown(i))) / baseCd * 255);
        }
    }

    if (flags.Conversation)
    {
        Conversation const* self = ToConversation();
        if (data->WriteBit(self->GetTextureKitId() != 0))
            *data << uint32(self->GetTextureKitId());

        data->FlushBits();
    }
}

UF::UpdateFieldFlag Object::GetUpdateFieldFlagsFor(Player const* /*target*/) const
{
    return UF::UpdateFieldFlag::None;
}

void Object::BuildValuesUpdateWithFlag(ByteBuffer* data, UF::UpdateFieldFlag /*flags*/, Player const* /*target*/) const
{
    std::size_t sizePos = data->wpos();
    *data << uint32(0);
    *data << uint32(0);

    data->put<uint32>(sizePos, data->wpos() - sizePos - 4);
}

void Object::AddToObjectUpdateIfNeeded()
{
    if (m_inWorld && !m_objectUpdated)
    {
        AddToObjectUpdate();
        m_objectUpdated = true;
    }
}

void Object::ClearUpdateMask(bool remove)
{
    m_values.ClearChangesMask(&Object::m_objectData);

    if (m_objectUpdated)
    {
        if (remove)
            RemoveFromObjectUpdate();
        m_objectUpdated = false;
    }
}

void Object::BuildFieldsUpdate(Player* player, UpdateDataMapType& data_map) const
{
    UpdateDataMapType::iterator iter = data_map.find(player);

    if (iter == data_map.end())
    {
        std::pair<UpdateDataMapType::iterator, bool> p = data_map.emplace(player, UpdateData(player->GetMapId()));
        ASSERT(p.second);
        iter = p.first;
    }

    BuildValuesUpdateBlockForPlayer(&iter->second, iter->first);
}

void MovementInfo::OutDebug()
{
    TC_LOG_DEBUG("misc", "MOVEMENT INFO");
    TC_LOG_DEBUG("misc", "%s", guid.ToString().c_str());
    TC_LOG_DEBUG("misc", "flags %s (%u)", Movement::MovementFlags_ToString(flags).c_str(), flags);
    TC_LOG_DEBUG("misc", "flags2 %s (%u)", Movement::MovementFlagsExtra_ToString(flags2).c_str(), flags2);
    TC_LOG_DEBUG("misc", "time %u current time %u", time, getMSTime());
    TC_LOG_DEBUG("misc", "position: `%s`", pos.ToString().c_str());
    if (!transport.guid.IsEmpty())
    {
        TC_LOG_DEBUG("misc", "TRANSPORT:");
        TC_LOG_DEBUG("misc", "%s", transport.guid.ToString().c_str());
        TC_LOG_DEBUG("misc", "position: `%s`", transport.pos.ToString().c_str());
        TC_LOG_DEBUG("misc", "seat: %i", transport.seat);
        TC_LOG_DEBUG("misc", "time: %u", transport.time);
        if (transport.prevTime)
            TC_LOG_DEBUG("misc", "prevTime: %u", transport.prevTime);
        if (transport.vehicleId)
            TC_LOG_DEBUG("misc", "vehicleId: %u", transport.vehicleId);
    }

    if ((flags & (MOVEMENTFLAG_SWIMMING | MOVEMENTFLAG_FLYING)) || (flags2 & MOVEMENTFLAG2_ALWAYS_ALLOW_PITCHING))
        TC_LOG_DEBUG("misc", "pitch: %f", pitch);

    if (flags & MOVEMENTFLAG_FALLING || jump.fallTime)
    {
        TC_LOG_DEBUG("misc", "fallTime: %u j_zspeed: %f", jump.fallTime, jump.zspeed);
        if (flags & MOVEMENTFLAG_FALLING)
            TC_LOG_DEBUG("misc", "j_sinAngle: %f j_cosAngle: %f j_xyspeed: %f", jump.sinAngle, jump.cosAngle, jump.xyspeed);
    }

    if (flags & MOVEMENTFLAG_SPLINE_ELEVATION)
        TC_LOG_DEBUG("misc", "splineElevation: %f", splineElevation);
}

WorldObject::WorldObject(bool isWorldObject) : WorldLocation(), LastUsedScriptID(0),
m_name(""), m_isActive(false), m_isWorldObject(isWorldObject), m_zoneScript(nullptr),
m_transport(nullptr), m_zoneId(0), m_areaId(0), m_staticFloorZ(VMAP_INVALID_HEIGHT), m_currMap(nullptr), m_InstanceId(0),
_dbPhase(0), m_notifyflags(0)
{
    m_serverSideVisibility.SetValue(SERVERSIDE_VISIBILITY_GHOST, GHOST_VISIBILITY_ALIVE | GHOST_VISIBILITY_GHOST);
    m_serverSideVisibilityDetect.SetValue(SERVERSIDE_VISIBILITY_GHOST, GHOST_VISIBILITY_ALIVE);
}

void WorldObject::SetWorldObject(bool on)
{
    if (!IsInWorld())
        return;

    GetMap()->AddObjectToSwitchList(this, on);
}

bool WorldObject::IsWorldObject() const
{
    if (m_isWorldObject)
        return true;

    if (ToCreature() && ToCreature()->m_isTempWorldObject)
        return true;

    return false;
}

void WorldObject::setActive(bool on)
{
    if (m_isActive == on)
        return;

    if (GetTypeId() == TYPEID_PLAYER)
        return;

    m_isActive = on;

    if (!IsInWorld())
        return;

    Map* map = FindMap();
    if (!map)
        return;

    if (on)
    {
        if (GetTypeId() == TYPEID_UNIT)
            map->AddToActive(ToCreature());
        else if (GetTypeId() == TYPEID_DYNAMICOBJECT)
            map->AddToActive((DynamicObject*)this);
    }
    else
    {
        if (GetTypeId() == TYPEID_UNIT)
            map->RemoveFromActive(ToCreature());
        else if (GetTypeId() == TYPEID_DYNAMICOBJECT)
            map->RemoveFromActive((DynamicObject*)this);
    }
}

void WorldObject::SetVisibilityDistanceOverride(VisibilityDistanceType type)
{
    ASSERT(type < VisibilityDistanceType::Max);
    if (GetTypeId() == TYPEID_PLAYER)
        return;

    m_visibilityDistanceOverride = VisibilityDistances[AsUnderlyingType(type)];
}

void WorldObject::CleanupsBeforeDelete(bool /*finalCleanup*/)
{
    if (IsInWorld())
        RemoveFromWorld();

    if (Transport* transport = GetTransport())
        transport->RemovePassenger(this);
}

void WorldObject::UpdatePositionData()
{
    PositionFullTerrainStatus data;
    GetMap()->GetFullTerrainStatusForPosition(_phaseShift, GetPositionX(), GetPositionY(), GetPositionZ(), data);
    ProcessPositionDataChanged(data);
}

void WorldObject::ProcessPositionDataChanged(PositionFullTerrainStatus const& data)
{
    m_zoneId = m_areaId = data.areaId;
    if (AreaTableEntry const* area = sAreaTableStore.LookupEntry(m_areaId))
        if (area->ParentAreaID)
            m_zoneId = area->ParentAreaID;
    m_staticFloorZ = data.floorZ;
}

void WorldObject::AddToWorld()
{
    Object::AddToWorld();
    GetMap()->GetZoneAndAreaId(_phaseShift, m_zoneId, m_areaId, GetPositionX(), GetPositionY(), GetPositionZ());
}

void WorldObject::RemoveFromWorld()
{
    if (!IsInWorld())
        return;

    DestroyForNearbyPlayers();

    Object::RemoveFromWorld();
}

bool WorldObject::IsInWorldPvpZone() const
{
    switch (GetZoneId())
    {
        case 4197: // Wintergrasp
        case 5095: // Tol Barad
        case 6941: // Ashran
            return true;
            break;
        default:
            return false;
            break;
    }
}

InstanceScript* WorldObject::GetInstanceScript() const
{
    Map* map = GetMap();
    return map->IsDungeon() ? ((InstanceMap*)map)->GetInstanceScript() : nullptr;
}

float WorldObject::GetDistanceZ(WorldObject const* obj) const
{
    float dz = std::fabs(GetPositionZ() - obj->GetPositionZ());
    float sizefactor = GetCombatReach() + obj->GetCombatReach();
    float dist = dz - sizefactor;
    return (dist > 0 ? dist : 0);
}

bool WorldObject::_IsWithinDist(WorldObject const* obj, float dist2compare, bool is3D, bool incOwnRadius, bool incTargetRadius) const
{
    float sizefactor = 0;
    sizefactor += incOwnRadius ? GetCombatReach() : 0.0f;
    sizefactor += incTargetRadius ? obj->GetCombatReach() : 0.0f;
    float maxdist = dist2compare + sizefactor;

    Position const* thisOrTransport = this;
    Position const* objOrObjTransport = obj;

    if (GetTransport() && obj->GetTransport() && obj->GetTransport()->GetGUID() == GetTransport()->GetGUID())
    {
        thisOrTransport = &m_movementInfo.transport.pos;
        objOrObjTransport = &obj->m_movementInfo.transport.pos;
    }

    if (is3D)
        return thisOrTransport->IsInDist(objOrObjTransport, maxdist);
    else
        return thisOrTransport->IsInDist2d(objOrObjTransport, maxdist);
}

float WorldObject::GetDistance(WorldObject const* obj) const
{
    float d = GetExactDist(obj) - GetCombatReach() - obj->GetCombatReach();
    return d > 0.0f ? d : 0.0f;
}

float WorldObject::GetDistance(Position const& pos) const
{
    float d = GetExactDist(&pos) - GetCombatReach();
    return d > 0.0f ? d : 0.0f;
}

float WorldObject::GetDistance(float x, float y, float z) const
{
    float d = GetExactDist(x, y, z) - GetCombatReach();
    return d > 0.0f ? d : 0.0f;
}

float WorldObject::GetDistance2d(WorldObject const* obj) const
{
    float d = GetExactDist2d(obj) - GetCombatReach() - obj->GetCombatReach();
    return d > 0.0f ? d : 0.0f;
}

float WorldObject::GetDistance2d(float x, float y) const
{
    float d = GetExactDist2d(x, y) - GetCombatReach();
    return d > 0.0f ? d : 0.0f;
}

bool WorldObject::IsSelfOrInSameMap(WorldObject const* obj) const
{
    if (this == obj)
        return true;
    return IsInMap(obj);
}

bool WorldObject::IsInMap(WorldObject const* obj) const
{
    if (obj)
        return IsInWorld() && obj->IsInWorld() && (GetMap() == obj->GetMap());
    return false;
}

bool WorldObject::IsWithinDist3d(float x, float y, float z, float dist) const
{
    return IsInDist(x, y, z, dist + GetCombatReach());
}

bool WorldObject::IsWithinDist3d(Position const* pos, float dist) const
{
    return IsInDist(pos, dist + GetCombatReach());
}

bool WorldObject::IsWithinDist2d(float x, float y, float dist) const
{
    return IsInDist2d(x, y, dist + GetCombatReach());
}

bool WorldObject::IsWithinDist2d(Position const* pos, float dist) const
{
    return IsInDist2d(pos, dist + GetCombatReach());
}

bool WorldObject::IsWithinDist(WorldObject const* obj, float dist2compare, bool is3D /*= true*/) const
{
    return obj && _IsWithinDist(obj, dist2compare, is3D);
}

bool WorldObject::IsWithinDistInMap(WorldObject const* obj, float dist2compare, bool is3D /*= true*/, bool incOwnRadius /*= true*/, bool incTargetRadius /*= true*/) const
{
    return obj && IsInMap(obj) && IsInPhase(obj) && _IsWithinDist(obj, dist2compare, is3D, incOwnRadius, incTargetRadius);
}

Position WorldObject::GetHitSpherePointFor(Position const& dest) const
{
    G3D::Vector3 vThis(GetPositionX(), GetPositionY(), GetPositionZ() + GetMidsectionHeight());
    G3D::Vector3 vObj(dest.GetPositionX(), dest.GetPositionY(), dest.GetPositionZ());
    G3D::Vector3 contactPoint = vThis + (vObj - vThis).directionOrZero() * std::min(dest.GetExactDist(GetPosition()), GetCombatReach());

    return Position(contactPoint.x, contactPoint.y, contactPoint.z, GetAngle(contactPoint.x, contactPoint.y));
}

bool WorldObject::IsWithinLOS(float ox, float oy, float oz, LineOfSightChecks checks, VMAP::ModelIgnoreFlags ignoreFlags) const
{
    if (IsInWorld())
    {
        float x, y, z;
        if (GetTypeId() == TYPEID_PLAYER)
        {
            GetPosition(x, y, z);
            z += GetMidsectionHeight();
        }
        else
            GetHitSpherePointFor({ ox, oy, oz }, x, y, z);

        return GetMap()->isInLineOfSight(GetPhaseShift(), x, y, z, ox, oy, oz, checks, ignoreFlags);
    }

    return true;
}

bool WorldObject::IsWithinLOSInMap(WorldObject const* obj, LineOfSightChecks checks, VMAP::ModelIgnoreFlags ignoreFlags) const
{
    if (!IsInMap(obj))
        return false;

    float x, y, z;
    if (obj->GetTypeId() == TYPEID_PLAYER)
    {
        obj->GetPosition(x, y, z);
        z += GetMidsectionHeight();
    }
    else
        obj->GetHitSpherePointFor({ GetPositionX(), GetPositionY(), GetPositionZ() + GetMidsectionHeight() }, x, y, z);

    return IsWithinLOS(x, y, z, checks, ignoreFlags);
}

void WorldObject::GetHitSpherePointFor(Position const& dest, float& x, float& y, float& z) const
{
    Position pos = GetHitSpherePointFor(dest);
    x = pos.GetPositionX();
    y = pos.GetPositionY();
    z = pos.GetPositionZ();
}

bool WorldObject::GetDistanceOrder(WorldObject const* obj1, WorldObject const* obj2, bool is3D /* = true */) const
{
    float dx1 = GetPositionX() - obj1->GetPositionX();
    float dy1 = GetPositionY() - obj1->GetPositionY();
    float distsq1 = dx1*dx1 + dy1*dy1;
    if (is3D)
    {
        float dz1 = GetPositionZ() - obj1->GetPositionZ();
        distsq1 += dz1*dz1;
    }

    float dx2 = GetPositionX() - obj2->GetPositionX();
    float dy2 = GetPositionY() - obj2->GetPositionY();
    float distsq2 = dx2*dx2 + dy2*dy2;
    if (is3D)
    {
        float dz2 = GetPositionZ() - obj2->GetPositionZ();
        distsq2 += dz2*dz2;
    }

    return distsq1 < distsq2;
}

bool WorldObject::IsInRange(WorldObject const* obj, float minRange, float maxRange, bool is3D /* = true */) const
{
    float dx = GetPositionX() - obj->GetPositionX();
    float dy = GetPositionY() - obj->GetPositionY();
    float distsq = dx*dx + dy*dy;
    if (is3D)
    {
        float dz = GetPositionZ() - obj->GetPositionZ();
        distsq += dz*dz;
    }

    float sizefactor = GetCombatReach() + obj->GetCombatReach();

    // check only for real range
    if (minRange > 0.0f)
    {
        float mindist = minRange + sizefactor;
        if (distsq < mindist * mindist)
            return false;
    }

    float maxdist = maxRange + sizefactor;
    return distsq < maxdist * maxdist;
}

bool WorldObject::IsInRange2d(float x, float y, float minRange, float maxRange) const
{
    float dx = GetPositionX() - x;
    float dy = GetPositionY() - y;
    float distsq = dx*dx + dy*dy;

    float sizefactor = GetCombatReach();

    // check only for real range
    if (minRange > 0.0f)
    {
        float mindist = minRange + sizefactor;
        if (distsq < mindist * mindist)
            return false;
    }

    float maxdist = maxRange + sizefactor;
    return distsq < maxdist * maxdist;
}

bool WorldObject::IsInRange3d(float x, float y, float z, float minRange, float maxRange) const
{
    float dx = GetPositionX() - x;
    float dy = GetPositionY() - y;
    float dz = GetPositionZ() - z;
    float distsq = dx*dx + dy*dy + dz*dz;

    float sizefactor = GetCombatReach();

    // check only for real range
    if (minRange > 0.0f)
    {
        float mindist = minRange + sizefactor;
        if (distsq < mindist * mindist)
            return false;
    }

    float maxdist = maxRange + sizefactor;
    return distsq < maxdist * maxdist;
}

bool WorldObject::IsInBetween(Position const& pos1, Position const& pos2, float size) const
{
    float dist = GetExactDist2d(pos1);

    // not using sqrt() for performance
    if ((dist * dist) >= pos1.GetExactDist2dSq(pos2))
        return false;

    if (!size)
        size = GetCombatReach() / 2;

    float angle = pos1.GetAngle(pos2);

    // not using sqrt() for performance
    return (size * size) >= GetExactDist2dSq(pos1.GetPositionX() + std::cos(angle) * dist, pos1.GetPositionY() + std::sin(angle) * dist);
}

bool WorldObject::isInFront(WorldObject const* target,  float arc) const
{
    return HasInArc(arc, target);
}

bool WorldObject::isInBack(WorldObject const* target, float arc) const
{
    return !HasInArc(2 * float(M_PI) - arc, target);
}

void WorldObject::GetRandomPoint(Position const& pos, float distance, float& rand_x, float& rand_y, float& rand_z) const
{
    if (!distance)
    {
        pos.GetPosition(rand_x, rand_y, rand_z);
        return;
    }

    // angle to face `obj` to `this`
    float angle = (float)rand_norm()*static_cast<float>(2*M_PI);
    float new_dist = (float)rand_norm() + (float)rand_norm();
    new_dist = distance * (new_dist > 1 ? new_dist - 2 : new_dist);

    rand_x = pos.m_positionX + new_dist * std::cos(angle);
    rand_y = pos.m_positionY + new_dist * std::sin(angle);
    rand_z = pos.m_positionZ;

    Trinity::NormalizeMapCoord(rand_x);
    Trinity::NormalizeMapCoord(rand_y);
    UpdateGroundPositionZ(rand_x, rand_y, rand_z);            // update to LOS height if available
}

Position WorldObject::GetRandomPoint(Position const& srcPos, float distance) const
{
    float x, y, z;
    GetRandomPoint(srcPos, distance, x, y, z);
    return Position(x, y, z, GetOrientation());
}

void WorldObject::UpdateGroundPositionZ(float x, float y, float &z) const
{
    float new_z = GetMap()->GetHeight(GetPhaseShift(), x, y, z + 2.0f, true);
    if (new_z > INVALID_HEIGHT)
        z = new_z + 0.05f;                                   // just to be sure that we are not a few pixel under the surface
}

void WorldObject::UpdateAllowedPositionZ(float x, float y, float &z) const
{
    // TODO: Allow transports to be part of dynamic vmap tree
    if (GetTransport())
        return;

    switch (GetTypeId())
    {
        case TYPEID_UNIT:
        {
            // non fly unit don't must be in air
            // non swim unit must be at ground (mostly speedup, because it don't must be in water and water level check less fast
            if (!ToCreature()->CanFly())
            {
                bool canSwim = ToCreature()->CanSwim();
                float ground_z = z;
                float max_z = canSwim
                    ? GetMap()->GetWaterOrGroundLevel(GetPhaseShift(), x, y, z, &ground_z, !ToUnit()->HasAuraType(SPELL_AURA_WATER_WALK))
                    : ((ground_z = GetMap()->GetHeight(GetPhaseShift(), x, y, z, true)));
                if (max_z > INVALID_HEIGHT)
                {
                    if (z > max_z)
                        z = max_z;
                    else if (z < ground_z)
                        z = ground_z;
                }
            }
            else
            {
                float ground_z = GetMap()->GetHeight(GetPhaseShift(), x, y, z, true);
                if (z < ground_z)
                    z = ground_z;
            }
            break;
        }
        case TYPEID_PLAYER:
        {
            // for server controlled moves playr work same as creature (but it can always swim)
            if (!ToPlayer()->CanFly())
            {
                float ground_z = z;
                float max_z = GetMap()->GetWaterOrGroundLevel(GetPhaseShift(), x, y, z, &ground_z, !ToUnit()->HasAuraType(SPELL_AURA_WATER_WALK));
                if (max_z > INVALID_HEIGHT)
                {
                    if (z > max_z)
                        z = max_z;
                    else if (z < ground_z)
                        z = ground_z;
                }
            }
            else
            {
                float ground_z = GetMap()->GetHeight(GetPhaseShift(), x, y, z, true);
                if (z < ground_z)
                    z = ground_z;
            }
            break;
        }
        default:
        {
            float ground_z = GetMap()->GetHeight(GetPhaseShift(), x, y, z, true);
            if (ground_z > INVALID_HEIGHT)
                z = ground_z;
            break;
        }
    }
}

float WorldObject::GetGridActivationRange() const
{
    if (isActiveObject())
    {
        if (GetTypeId() == TYPEID_PLAYER && ToPlayer()->GetCinematicMgr()->IsOnCinematic())
            return std::max(DEFAULT_VISIBILITY_INSTANCE, GetMap()->GetVisibilityRange());

        return GetMap()->GetVisibilityRange();
    }

    if (Creature const* thisCreature = ToCreature())
        return thisCreature->m_SightDistance;

    return 0.0f;
}

float WorldObject::GetVisibilityRange() const
{
    if (IsVisibilityOverridden() && !ToPlayer())
        return *m_visibilityDistanceOverride;
    else if (isActiveObject() && !ToPlayer())
        return MAX_VISIBILITY_DISTANCE;
    else
        return GetMap()->GetVisibilityRange();
}

float WorldObject::GetSightRange(WorldObject const* target) const
{
    if (ToUnit())
    {
        if (ToPlayer())
        {
            if (target && target->IsVisibilityOverridden() && !target->ToPlayer())
                return *target->m_visibilityDistanceOverride;
            else if (target && target->isActiveObject() && !target->ToPlayer())
                return MAX_VISIBILITY_DISTANCE;
            else if (ToPlayer()->GetCinematicMgr()->IsOnCinematic())
                return DEFAULT_VISIBILITY_INSTANCE;
            else
                return GetMap()->GetVisibilityRange();
        }
        else if (ToCreature())
            return ToCreature()->m_SightDistance;
        else
            return SIGHT_RANGE_UNIT;
    }

    if (ToDynObject() && isActiveObject())
    {
        return GetMap()->GetVisibilityRange();
    }

    return 0.0f;
}

bool WorldObject::CanSeeOrDetect(WorldObject const* obj, bool ignoreStealth, bool distanceCheck, bool checkAlert) const
{
    if (this == obj)
        return true;

    if (obj->IsNeverVisibleFor(this) || CanNeverSee(obj))
        return false;

    if (obj->IsAlwaysVisibleFor(this) || CanAlwaysSee(obj))
        return true;

    bool corpseVisibility = false;
    if (distanceCheck)
    {
        bool corpseCheck = false;
        if (Player const* thisPlayer = ToPlayer())
        {
            if (thisPlayer->isDead() && thisPlayer->GetHealth() > 0 && // Cheap way to check for ghost state
                !(obj->m_serverSideVisibility.GetValue(SERVERSIDE_VISIBILITY_GHOST) & m_serverSideVisibility.GetValue(SERVERSIDE_VISIBILITY_GHOST) & GHOST_VISIBILITY_GHOST))
            {
                if (Corpse* corpse = thisPlayer->GetCorpse())
                {
                    corpseCheck = true;
                    if (corpse->IsWithinDist(thisPlayer, GetSightRange(obj), false))
                        if (corpse->IsWithinDist(obj, GetSightRange(obj), false))
                            corpseVisibility = true;
                }
            }

            if (Unit const* target = obj->ToUnit())
            {
                // Don't allow to detect vehicle accessories if you can't see vehicle
                if (Unit const* vehicle = target->GetVehicleBase())
                    if (!thisPlayer->HaveAtClient(vehicle))
                        return false;
            }
        }

        WorldObject const* viewpoint = this;
        if (Player const* player = ToPlayer())
        {
            viewpoint = player->GetViewpoint();

            if (Creature const* creature = obj->ToCreature())
                if (TempSummon::IsPersonalSummonOfAnotherPlayer(creature, GetGUID()))
                    return false;
        }

        if (GameObject const* go = obj->ToGameObject())
            if (go->IsVisibleByUnitOnly() && GetGUID() != go->GetVisibleByUnitOnly())
                return false;

        if (!viewpoint)
            viewpoint = this;

        if (!corpseCheck && !viewpoint->IsWithinDist(obj, GetSightRange(obj), false))
            return false;
    }

    // GM visibility off or hidden NPC
    if (!obj->m_serverSideVisibility.GetValue(SERVERSIDE_VISIBILITY_GM))
    {
        // Stop checking other things for GMs
        if (m_serverSideVisibilityDetect.GetValue(SERVERSIDE_VISIBILITY_GM))
            return true;
    }
    else
        return m_serverSideVisibilityDetect.GetValue(SERVERSIDE_VISIBILITY_GM) >= obj->m_serverSideVisibility.GetValue(SERVERSIDE_VISIBILITY_GM);

    // Ghost players, Spirit Healers, and some other NPCs
    if (!corpseVisibility && !(obj->m_serverSideVisibility.GetValue(SERVERSIDE_VISIBILITY_GHOST) & m_serverSideVisibilityDetect.GetValue(SERVERSIDE_VISIBILITY_GHOST)))
    {
        // Alive players can see dead players in some cases, but other objects can't do that
        if (Player const* thisPlayer = ToPlayer())
        {
            if (Player const* objPlayer = obj->ToPlayer())
            {
                if (thisPlayer->GetTeam() != objPlayer->GetTeam() || !thisPlayer->IsGroupVisibleFor(objPlayer))
                    return false;
            }
            else
                return false;
        }
        else
            return false;
    }

    if (obj->IsInvisibleDueToDespawn())
        return false;

    if (!CanDetect(obj, ignoreStealth, checkAlert))
        return false;

    return true;
}

bool WorldObject::CanNeverSee(WorldObject const* obj) const
{
    return GetMap() != obj->GetMap() || !IsInPhase(obj);
}

bool WorldObject::CanDetect(WorldObject const* obj, bool ignoreStealth, bool checkAlert) const
{
    WorldObject const* seer = this;

    // Pets don't have detection, they use the detection of their masters
    if (Unit const* thisUnit = ToUnit())
        if (Unit* controller = thisUnit->GetCharmerOrOwner())
            seer = controller;

    if (obj->IsAlwaysDetectableFor(seer))
        return true;

    if (!ignoreStealth && !seer->CanDetectInvisibilityOf(obj))
        return false;

    if (!ignoreStealth && !seer->CanDetectStealthOf(obj, checkAlert))
        return false;

    return true;
}

bool WorldObject::CanDetectInvisibilityOf(WorldObject const* obj) const
{
    uint32 mask = obj->m_invisibility.GetFlags() & m_invisibilityDetect.GetFlags();

    // Check for not detected types
    if (mask != obj->m_invisibility.GetFlags())
        return false;

    for (uint32 i = 0; i < TOTAL_INVISIBILITY_TYPES; ++i)
    {
        if (!(mask & (1 << i)))
            continue;

        int32 objInvisibilityValue = obj->m_invisibility.GetValue(InvisibilityType(i));
        int32 ownInvisibilityDetectValue = m_invisibilityDetect.GetValue(InvisibilityType(i));

        // Too low value to detect
        if (ownInvisibilityDetectValue < objInvisibilityValue)
            return false;
    }

    return true;
}

bool WorldObject::CanDetectStealthOf(WorldObject const* obj, bool checkAlert) const
{
    // Combat reach is the minimal distance (both in front and behind),
    //   and it is also used in the range calculation.
    // One stealth point increases the visibility range by 0.3 yard.

    if (!obj->m_stealth.GetFlags())
        return true;

    float distance = GetExactDist(obj);
    float combatReach = 0.0f;

    Unit const* unit = ToUnit();
    if (unit)
        combatReach = unit->GetCombatReach();

    if (distance < combatReach)
        return true;

    if (!HasInArc(float(M_PI), obj))
        return false;

    GameObject const* go = obj->ToGameObject();
    for (uint32 i = 0; i < TOTAL_STEALTH_TYPES; ++i)
    {
        if (!(obj->m_stealth.GetFlags() & (1 << i)))
            continue;

        if (unit && unit->HasAuraTypeWithMiscvalue(SPELL_AURA_DETECT_STEALTH, i))
            return true;

        // Starting points
        int32 detectionValue = 30;

        // Level difference: 5 point / level, starting from level 1.
        // There may be spells for this and the starting points too, but
        // not in the DBCs of the client.
        detectionValue += int32(GetLevelForTarget(obj) - 1) * 5;

        // Apply modifiers
        detectionValue += m_stealthDetect.GetValue(StealthType(i));
        if (go)
            if (Unit* owner = go->GetOwner())
                detectionValue -= int32(owner->GetLevelForTarget(this) - 1) * 5;

        detectionValue -= obj->m_stealth.GetValue(StealthType(i));

        // Calculate max distance
        float visibilityRange = float(detectionValue) * 0.3f + combatReach;

        // If this unit is an NPC then player detect range doesn't apply
        if (unit && unit->GetTypeId() == TYPEID_PLAYER && visibilityRange > MAX_PLAYER_STEALTH_DETECT_RANGE)
            visibilityRange = MAX_PLAYER_STEALTH_DETECT_RANGE;

        // When checking for alert state, look 8% further, and then 1.5 yards more than that.
        if (checkAlert)
            visibilityRange += (visibilityRange * 0.08f) + 1.5f;

        // If checking for alert, and creature's visibility range is greater than aggro distance, No alert
        Unit const* tunit = obj->ToUnit();
        if (checkAlert && unit && unit->ToCreature() && visibilityRange >= unit->ToCreature()->GetAttackDistance(tunit) + unit->ToCreature()->m_CombatDistance)
            return false;

        if (distance > visibilityRange)
            return false;
    }

    return true;
}

void WorldObject::SendMessageToSet(WorldPacket const* data, bool self) const
{
    if (IsInWorld())
        SendMessageToSetInRange(data, GetVisibilityRange(), self);
}

void WorldObject::SendMessageToSetInRange(WorldPacket const* data, float dist, bool /*self*/) const
{
    Trinity::MessageDistDeliverer notifier(this, data, dist);
    Cell::VisitWorldObjects(this, notifier, dist);
}

void WorldObject::SendMessageToSet(WorldPacket const* data, Player const* skipped_rcvr) const
{
    Trinity::MessageDistDeliverer notifier(this, data, GetVisibilityRange(), false, skipped_rcvr);
    Cell::VisitWorldObjects(this, notifier, GetVisibilityRange());
}

void WorldObject::SetMap(Map* map)
{
    ASSERT(map);
    ASSERT(!IsInWorld());
    if (m_currMap == map) // command add npc: first create, than loadfromdb
        return;
    if (m_currMap)
    {
        TC_LOG_FATAL("misc", "WorldObject::SetMap: obj %u new map %u %u, old map %u %u", (uint32)GetTypeId(), map->GetId(), map->GetInstanceId(), m_currMap->GetId(), m_currMap->GetInstanceId());
        ABORT();
    }
    m_currMap = map;
    m_mapId = map->GetId();
    m_InstanceId = map->GetInstanceId();
    if (IsWorldObject())
        m_currMap->AddWorldObject(this);
}

void WorldObject::ResetMap()
{
    ASSERT(m_currMap);
    ASSERT(!IsInWorld());
    if (IsWorldObject())
        m_currMap->RemoveWorldObject(this);
    m_currMap = nullptr;
    //maybe not for corpse
    //m_mapId = 0;
    //m_InstanceId = 0;
}

void WorldObject::AddObjectToRemoveList()
{
    Map* map = FindMap();
    if (!map)
    {
        TC_LOG_ERROR("misc", "Object (Entry: %u %s) at attempt add to move list not have valid map (Id: %u).", GetEntry(), GetGUID().ToString().c_str(), GetMapId());
        return;
    }

    map->AddObjectToRemoveList(this);
}

TempSummon* Map::SummonCreature(uint32 entry, Position const& pos, SummonPropertiesEntry const* properties /*= nullptr*/, uint32 duration /*= 0*/, Unit* summoner /*= nullptr*/, uint32 spellId /*= 0*/, uint32 vehId /*= 0*/, bool visibleBySummonerOnly /*= false*/)
{
    uint32 mask = UNIT_MASK_SUMMON;
    if (properties)
    {
        switch (properties->Control)
        {
            case SUMMON_CATEGORY_PET:
                mask = UNIT_MASK_GUARDIAN;
                break;
            case SUMMON_CATEGORY_PUPPET:
                mask = UNIT_MASK_PUPPET;
                break;
            case SUMMON_CATEGORY_VEHICLE:
                mask = UNIT_MASK_MINION;
                break;
            case SUMMON_CATEGORY_WILD:
            case SUMMON_CATEGORY_ALLY:
            case SUMMON_CATEGORY_UNK:
            {
                switch (SummonTitle(properties->Title))
                {
                case SummonTitle::Minion:
                case SummonTitle::Guardian:
                case SummonTitle::Runeblade:
                    mask = UNIT_MASK_GUARDIAN;
                    break;
                case SummonTitle::Totem:
                case SummonTitle::Lightwell:
                    mask = UNIT_MASK_TOTEM;
                    break;
                case SummonTitle::Vehicle:
                case SummonTitle::Mount:
                    mask = UNIT_MASK_SUMMON;
                    break;
                case SummonTitle::Companion:
                    mask = UNIT_MASK_MINION;
                    break;
                default:
                    if (properties->Flags & 512) // Mirror Image, Summon Gargoyle
                        mask = UNIT_MASK_GUARDIAN;
                    break;
                }
                break;
            }
            default:
                return nullptr;
        }
    }

    TempSummon* summon = nullptr;
    switch (mask)
    {
        case UNIT_MASK_SUMMON:
            summon = new TempSummon(properties, summoner, false);
            break;
        case UNIT_MASK_GUARDIAN:
            summon = new Guardian(properties, summoner, false);
            break;
        case UNIT_MASK_PUPPET:
            summon = new Puppet(properties, summoner);
            break;
        case UNIT_MASK_TOTEM:
            summon = new Totem(properties, summoner);
            break;
        case UNIT_MASK_MINION:
            summon = new Minion(properties, summoner, false);
            break;
    }

    if (!summon->Create(GenerateLowGuid<HighGuid::Creature>(), this, entry, pos, nullptr, vehId, true))
    {
        delete summon;
        return nullptr;
    }

    // Set the summon to the summoner's phase
    if (summoner)
        PhasingHandler::InheritPhaseShift(summon, summoner);

    summon->SetCreatedBySpell(spellId);

    summon->SetHomePosition(pos);

    summon->InitStats(duration);

    summon->SetVisibleBySummonerOnly(visibleBySummonerOnly);

    AddToMap(summon->ToCreature());
    summon->InitSummon();

    // call MoveInLineOfSight for nearby creatures
    Trinity::AIRelocationNotifier notifier(*summon);
    Cell::VisitAllObjects(summon, notifier, GetVisibilityRange());

    return summon;
}

/**
* Summons group of creatures.
*
* @param group Id of group to summon.
* @param list  List to store pointers to summoned creatures.
*/

void Map::SummonCreatureGroup(uint8 group, std::list<TempSummon*>* list /*= nullptr*/)
{
    std::vector<TempSummonData> const* data = sObjectMgr->GetSummonGroup(GetId(), SUMMONER_TYPE_MAP, group);
    if (!data)
        return;

    for (std::vector<TempSummonData>::const_iterator itr = data->begin(); itr != data->end(); ++itr)
        if (TempSummon* summon = SummonCreature(itr->entry, itr->pos, nullptr, itr->time))
            if (list)
                list->push_back(summon);
}

void WorldObject::SetZoneScript()
{
    if (Map* map = FindMap())
    {
        if (map->IsDungeon())
            m_zoneScript = (ZoneScript*)((InstanceMap*)map)->GetInstanceScript();
        else if (!map->IsBattlegroundOrArena())
        {
            if (Battlefield* bf = sBattlefieldMgr->GetBattlefieldToZoneId(GetZoneId()))
                m_zoneScript = bf;
            else
                m_zoneScript = sOutdoorPvPMgr->GetZoneScript(GetZoneId());
        }
    }
}

Scenario* WorldObject::GetScenario() const
{
    if (IsInWorld())
        if (InstanceMap* instanceMap = GetMap()->ToInstanceMap())
            return instanceMap->GetInstanceScenario();

    return nullptr;
}

TempSummon* WorldObject::SummonCreature(uint32 entry, Position const& pos, TempSummonType despawnType /*= TEMPSUMMON_MANUAL_DESPAWN*/, uint32 despawnTime /*= 0*/, uint32 vehId /*= 0*/, bool visibleBySummonerOnly /*= false*/)
{
    if (Map* map = FindMap())
    {
        if (TempSummon* summon = map->SummonCreature(entry, pos, nullptr, despawnTime, ToUnit(), 0, vehId, visibleBySummonerOnly))
        {
            summon->SetTempSummonType(despawnType);
            return summon;
        }
    }

    return nullptr;
}

TempSummon* WorldObject::SummonCreature(uint32 id, float x, float y, float z, float o /*= 0*/, TempSummonType despawnType /*= TEMPSUMMON_MANUAL_DESPAWN*/, uint32 despawnTime /*= 0*/, bool visibleBySummonerOnly /*= false*/)
{
    if (!x && !y && !z)
        GetClosePoint(x, y, z, GetCombatReach());
    if (!o)
        o = GetOrientation();
    return SummonCreature(id, { x,y,z,o }, despawnType, despawnTime, 0, visibleBySummonerOnly);
}

GameObject* WorldObject::SummonGameObject(uint32 entry, Position const& pos, QuaternionData const& rot, uint32 respawnTime)
{
    if (!IsInWorld())
        return nullptr;

    GameObjectTemplate const* goinfo = sObjectMgr->GetGameObjectTemplate(entry);
    if (!goinfo)
    {
        TC_LOG_ERROR("sql.sql", "Gameobject template %u not found in database!", entry);
        return nullptr;
    }

    Map* map = GetMap();
    GameObject* go = GameObject::CreateGameObject(entry, map, pos, rot, 255, GO_STATE_READY);
    if (!go)
        return nullptr;

    PhasingHandler::InheritPhaseShift(go, this);

    go->SetRespawnTime(respawnTime);
    if (GetTypeId() == TYPEID_PLAYER || GetTypeId() == TYPEID_UNIT) //not sure how to handle this
        ToUnit()->AddGameObject(go);
    else
        go->SetSpawnedByDefault(false);

    map->AddToMap(go);
    return go;
}

GameObject* WorldObject::SummonGameObject(uint32 entry, float x, float y, float z, float ang, QuaternionData const& rot, uint32 respawnTime)
{
    if (!x && !y && !z)
    {
        GetClosePoint(x, y, z, GetCombatReach());
        ang = GetOrientation();
    }

    Position pos(x, y, z, ang);
    return SummonGameObject(entry, pos, rot, respawnTime);
}

Creature* WorldObject::SummonTrigger(float x, float y, float z, float ang, uint32 duration, CreatureAI* (*GetAI)(Creature*))
{
    TempSummonType summonType = (duration == 0) ? TEMPSUMMON_DEAD_DESPAWN : TEMPSUMMON_TIMED_DESPAWN;
    Creature* summon = SummonCreature(WORLD_TRIGGER, x, y, z, ang, summonType, duration);
    if (!summon)
        return nullptr;

    //summon->SetName(GetName());
    if (GetTypeId() == TYPEID_PLAYER || GetTypeId() == TYPEID_UNIT)
    {
        summon->SetFaction(((Unit*)this)->GetFaction());
        summon->SetLevel(((Unit*)this)->getLevel());
    }

    if (GetAI)
        summon->AIM_Initialize(GetAI(summon));
    return summon;
}

/**
* Summons group of creatures. Should be called only by instances of Creature and GameObject classes.
*
* @param group Id of group to summon.
* @param list  List to store pointers to summoned creatures.
*/
void WorldObject::SummonCreatureGroup(uint8 group, std::list<TempSummon*>* list /*= nullptr*/)
{
    ASSERT((GetTypeId() == TYPEID_GAMEOBJECT || GetTypeId() == TYPEID_UNIT) && "Only GOs and creatures can summon npc groups!");

    std::vector<TempSummonData> const* data = sObjectMgr->GetSummonGroup(GetEntry(), GetTypeId() == TYPEID_GAMEOBJECT ? SUMMONER_TYPE_GAMEOBJECT : SUMMONER_TYPE_CREATURE, group);
    if (!data)
    {
        TC_LOG_WARN("scripts", "%s (%s) tried to summon non-existing summon group %u.", GetName().c_str(), GetGUID().ToString().c_str(), group);
        return;
    }

    for (std::vector<TempSummonData>::const_iterator itr = data->begin(); itr != data->end(); ++itr)
        if (TempSummon* summon = SummonCreature(itr->entry, itr->pos, itr->type, itr->time))
            if (list)
                list->push_back(summon);
}

Creature* WorldObject::FindNearestCreature(uint32 entry, float range, bool alive) const
{
    Creature* creature = nullptr;
    Trinity::NearestCreatureEntryWithLiveStateInObjectRangeCheck checker(*this, entry, alive, range);
    Trinity::CreatureLastSearcher<Trinity::NearestCreatureEntryWithLiveStateInObjectRangeCheck> searcher(this, creature, checker);
    Cell::VisitAllObjects(this, searcher, range);
    return creature;
}

GameObject* WorldObject::FindNearestGameObject(uint32 entry, float range) const
{
    GameObject* go = nullptr;
    Trinity::NearestGameObjectEntryInObjectRangeCheck checker(*this, entry, range);
    Trinity::GameObjectLastSearcher<Trinity::NearestGameObjectEntryInObjectRangeCheck> searcher(this, go, checker);
    Cell::VisitGridObjects(this, searcher, range);
    return go;
}

GameObject* WorldObject::FindNearestGameObjectOfType(GameobjectTypes type, float range) const
{
    GameObject* go = nullptr;
    Trinity::NearestGameObjectTypeInObjectRangeCheck checker(*this, type, range);
    Trinity::GameObjectLastSearcher<Trinity::NearestGameObjectTypeInObjectRangeCheck> searcher(this, go, checker);
    Cell::VisitGridObjects(this, searcher, range);
    return go;
}

Player* WorldObject::SelectNearestPlayer(float distance) const
{
    Player* target = nullptr;

    Trinity::NearestPlayerInObjectRangeCheck checker(this, distance);
    Trinity::PlayerLastSearcher<Trinity::NearestPlayerInObjectRangeCheck> searcher(this, target, checker);
    Cell::VisitGridObjects(this, searcher, distance);

    return target;
}

template <typename Container>
void WorldObject::GetGameObjectListWithEntryInGrid(Container& gameObjectContainer, uint32 entry, float maxSearchRange /*= 250.0f*/) const
{
    Trinity::AllGameObjectsWithEntryInRange check(this, entry, maxSearchRange);
    Trinity::GameObjectListSearcher<Trinity::AllGameObjectsWithEntryInRange> searcher(this, gameObjectContainer, check);
    Cell::VisitGridObjects(this, searcher, maxSearchRange);
}

template <typename Container>
void WorldObject::GetCreatureListWithEntryInGrid(Container& creatureContainer, uint32 entry, float maxSearchRange /*= 250.0f*/) const
{
    Trinity::AllCreaturesOfEntryInRange check(this, entry, maxSearchRange);
    Trinity::CreatureListSearcher<Trinity::AllCreaturesOfEntryInRange> searcher(this, creatureContainer, check);
    Cell::VisitGridObjects(this, searcher, maxSearchRange);
}

template <typename Container>
void WorldObject::GetPlayerListInGrid(Container& playerContainer, float maxSearchRange) const
{
    Trinity::AnyPlayerInObjectRangeCheck checker(this, maxSearchRange);
    Trinity::PlayerListSearcher<Trinity::AnyPlayerInObjectRangeCheck> searcher(this, playerContainer, checker);
    Cell::VisitWorldObjects(this, searcher, maxSearchRange);
}

void WorldObject::GetNearPoint2D(float &x, float &y, float distance2d, float absAngle) const
{
    x = GetPositionX() + (GetCombatReach() + distance2d) * std::cos(absAngle);
    y = GetPositionY() + (GetCombatReach() + distance2d) * std::sin(absAngle);

    Trinity::NormalizeMapCoord(x);
    Trinity::NormalizeMapCoord(y);
}

void WorldObject::GetNearPoint(WorldObject const* /*searcher*/, float &x, float &y, float &z, float searcher_size, float distance2d, float absAngle) const
{
    GetNearPoint2D(x, y, distance2d+searcher_size, absAngle);
    z = GetPositionZ();
    // Should "searcher" be used instead of "this" when updating z coordinate ?
    UpdateAllowedPositionZ(x, y, z);

    // if detection disabled, return first point
    if (!sWorld->getBoolConfig(CONFIG_DETECT_POS_COLLISION))
        return;

    // return if the point is already in LoS
    if (IsWithinLOS(x, y, z))
        return;

    // remember first point
    float first_x = x;
    float first_y = y;
    float first_z = z;

    // loop in a circle to look for a point in LoS using small steps
    for (float angle = float(M_PI) / 8; angle < float(M_PI) * 2; angle += float(M_PI) / 8)
    {
        GetNearPoint2D(x, y, distance2d + searcher_size, absAngle + angle);
        z = GetPositionZ();
        UpdateAllowedPositionZ(x, y, z);
        if (IsWithinLOS(x, y, z))
            return;
    }

    // still not in LoS, give up and return first position found
    x = first_x;
    y = first_y;
    z = first_z;
}

void WorldObject::GetClosePoint(float &x, float &y, float &z, float size, float distance2d /*= 0*/, float angle /*= 0*/) const
{
    // angle calculated from current orientation
    GetNearPoint(nullptr, x, y, z, size, distance2d, GetOrientation() + angle);
}

Position WorldObject::GetNearPosition(float dist, float angle)
{
    Position pos = GetPosition();
    MovePosition(pos, dist, angle);
    return pos;
}

Position WorldObject::GetFirstCollisionPosition(float dist, float angle)
{
    Position pos = GetPosition();
    MovePositionToFirstCollision(pos, dist, angle);
    return pos;
}

Position WorldObject::GetRandomNearPosition(float radius)
{
    Position pos = GetPosition();
    MovePosition(pos, radius * (float)rand_norm(), (float)rand_norm() * static_cast<float>(2 * M_PI));
    return pos;
}

void WorldObject::GetContactPoint(WorldObject const* obj, float& x, float& y, float& z, float distance2d /*= CONTACT_DISTANCE*/) const
{
    // angle to face `obj` to `this` using distance includes size of `obj`
    GetNearPoint(obj, x, y, z, obj->GetCombatReach(), distance2d, GetAngle(obj));
}

void WorldObject::MovePosition(Position &pos, float dist, float angle)
{
    angle += GetOrientation();
    float destx, desty, destz, ground, floor;
    destx = pos.m_positionX + dist * std::cos(angle);
    desty = pos.m_positionY + dist * std::sin(angle);

    // Prevent invalid coordinates here, position is unchanged
    if (!Trinity::IsValidMapCoord(destx, desty, pos.m_positionZ))
    {
        TC_LOG_FATAL("misc", "WorldObject::MovePosition: Object (Entry: %u %s) has invalid coordinates X: %f and Y: %f were passed!",
            GetEntry(), GetGUID().ToString().c_str(), destx, desty);
        return;
    }

    ground = GetMap()->GetHeight(GetPhaseShift(), destx, desty, MAX_HEIGHT, true);
    floor = GetMap()->GetHeight(GetPhaseShift(), destx, desty, pos.m_positionZ, true);
    destz = std::fabs(ground - pos.m_positionZ) <= std::fabs(floor - pos.m_positionZ) ? ground : floor;

    float step = dist/10.0f;

    for (uint8 j = 0; j < 10; ++j)
    {
        // do not allow too big z changes
        if (std::fabs(pos.m_positionZ - destz) > 6)
        {
            destx -= step * std::cos(angle);
            desty -= step * std::sin(angle);
            ground = GetMap()->GetHeight(GetPhaseShift(), destx, desty, MAX_HEIGHT, true);
            floor = GetMap()->GetHeight(GetPhaseShift(), destx, desty, pos.m_positionZ, true);
            destz = std::fabs(ground - pos.m_positionZ) <= std::fabs(floor - pos.m_positionZ) ? ground : floor;
        }
        // we have correct destz now
        else
        {
            pos.Relocate(destx, desty, destz);
            break;
        }
    }

    Trinity::NormalizeMapCoord(pos.m_positionX);
    Trinity::NormalizeMapCoord(pos.m_positionY);
    UpdateGroundPositionZ(pos.m_positionX, pos.m_positionY, pos.m_positionZ);
    pos.SetOrientation(GetOrientation());
}

// @todo: replace with WorldObject::UpdateAllowedPositionZ
float NormalizeZforCollision(WorldObject* obj, float x, float y, float z)
{
    float ground = obj->GetMap()->GetHeight(obj->GetPhaseShift(), x, y, MAX_HEIGHT, true);
    float floor = obj->GetMap()->GetHeight(obj->GetPhaseShift(), x, y, z + 2.0f, true);
    float helper = std::fabs(ground - z) <= std::fabs(floor - z) ? ground : floor;
    if (z > helper) // must be above ground
    {
        if (Unit* unit = obj->ToUnit())
        {
            if (unit->CanFly())
                return z;
        }
        LiquidData liquid_status;
        ZLiquidStatus res = obj->GetMap()->GetLiquidStatus(obj->GetPhaseShift(), x, y, z, MAP_ALL_LIQUIDS, &liquid_status);
        if (res && liquid_status.level > helper) // water must be above ground
        {
            if (liquid_status.level > z) // z is underwater
                return z;
            else
                return std::fabs(liquid_status.level - z) <= std::fabs(helper - z) ? liquid_status.level : helper;
        }
    }
    return helper;
}

void WorldObject::MovePositionToFirstCollision(Position &pos, float dist, float angle)
{
    angle += GetOrientation();
    float destx, desty, destz;
    destx = pos.m_positionX + dist * std::cos(angle);
    desty = pos.m_positionY + dist * std::sin(angle);

    // Prevent invalid coordinates here, position is unchanged
    if (!Trinity::IsValidMapCoord(destx, desty))
    {
        TC_LOG_FATAL("misc", "WorldObject::MovePositionToFirstCollision invalid coordinates X: %f and Y: %f were passed!", destx, desty);
        return;
    }

    destz = NormalizeZforCollision(this, destx, desty, pos.GetPositionZ());
    bool col = VMAP::VMapFactory::createOrGetVMapManager()->getObjectHitPos(PhasingHandler::GetTerrainMapId(GetPhaseShift(), GetMap(), pos.m_positionX, pos.m_positionY),
        pos.m_positionX, pos.m_positionY, pos.m_positionZ + 0.5f, destx, desty, destz + 0.5f, destx, desty, destz, -0.5f);

    // collision occured
    if (col)
    {
        // move back a bit
        destx -= CONTACT_DISTANCE * std::cos(angle);
        desty -= CONTACT_DISTANCE * std::sin(angle);
        dist = std::sqrt((pos.m_positionX - destx)*(pos.m_positionX - destx) + (pos.m_positionY - desty)*(pos.m_positionY - desty));
    }

    // check dynamic collision
    col = GetMap()->getObjectHitPos(GetPhaseShift(), pos.m_positionX, pos.m_positionY, pos.m_positionZ + 0.5f, destx, desty, destz + 0.5f, destx, desty, destz, -0.5f);

    // Collided with a gameobject
    if (col)
    {
        destx -= CONTACT_DISTANCE * std::cos(angle);
        desty -= CONTACT_DISTANCE * std::sin(angle);
        dist = std::sqrt((pos.m_positionX - destx)*(pos.m_positionX - destx) + (pos.m_positionY - desty)*(pos.m_positionY - desty));
    }

    float step = dist / 10.0f;

    for (uint8 j = 0; j < 10; ++j)
    {
        // do not allow too big z changes
        if (std::fabs(pos.m_positionZ - destz) > 6.0f)
        {
            destx -= step * std::cos(angle);
            desty -= step * std::sin(angle);
            destz = NormalizeZforCollision(this, destx, desty, pos.GetPositionZ());
        }
        // we have correct destz now
        else
        {
            pos.Relocate(destx, desty, destz);
            break;
        }
    }

    Trinity::NormalizeMapCoord(pos.m_positionX);
    Trinity::NormalizeMapCoord(pos.m_positionY);
    pos.m_positionZ = NormalizeZforCollision(this, destx, desty, pos.GetPositionZ());
    pos.SetOrientation(GetOrientation());
}

void WorldObject::PlayDistanceSound(uint32 soundId, Player* target /*= nullptr*/)
{
    if (target)
        target->SendDirectMessage(WorldPackets::Misc::PlaySpeakerbotSound(GetGUID(), soundId).Write());
    else
        SendMessageToSet(WorldPackets::Misc::PlaySpeakerbotSound(GetGUID(), soundId).Write(), true);
}

void WorldObject::PlayDirectSound(uint32 soundId, Player* target /*= nullptr*/, uint32 broadcastTextId /*= 0*/)
{
    if (target)
        target->SendDirectMessage(WorldPackets::Misc::PlaySound(GetGUID(), soundId, broadcastTextId).Write());
    else
        SendMessageToSet(WorldPackets::Misc::PlaySound(GetGUID(), soundId, broadcastTextId).Write(), true);
}

void WorldObject::PlayDirectMusic(uint32 musicId, Player* target /*= nullptr*/)
{
    if (target)
        target->SendDirectMessage(WorldPackets::Misc::PlayMusic(musicId).Write());
    else
        SendMessageToSet(WorldPackets::Misc::PlayMusic(musicId).Write(), true);
}

void WorldObject::DestroyForNearbyPlayers()
{
    if (!IsInWorld())
        return;

    std::list<Player*> targets;
    Trinity::AnyPlayerInObjectRangeCheck check(this, GetVisibilityRange(), false);
    Trinity::PlayerListSearcher<Trinity::AnyPlayerInObjectRangeCheck> searcher(this, targets, check);
    Cell::VisitWorldObjects(this, searcher, GetVisibilityRange());
    for (std::list<Player*>::const_iterator iter = targets.begin(); iter != targets.end(); ++iter)
    {
        Player* player = (*iter);

        if (player == this)
            continue;

        if (!player->HaveAtClient(this))
            continue;

        if (isType(TYPEMASK_UNIT) && ToUnit()->GetCharmerGUID() == player->GetGUID()) /// @todo this is for puppet
            continue;

        DestroyForPlayer(player);
        player->m_clientGUIDs.erase(GetGUID());
    }
}

void WorldObject::UpdateObjectVisibility(bool /*forced*/)
{
    //updates object's visibility for nearby players
    Trinity::VisibleChangesNotifier notifier(*this);
    Cell::VisitWorldObjects(this, notifier, GetVisibilityRange());
}

struct WorldObjectChangeAccumulator
{
    UpdateDataMapType& i_updateDatas;
    WorldObject& i_object;
    GuidSet plr_list;
    WorldObjectChangeAccumulator(WorldObject &obj, UpdateDataMapType &d) : i_updateDatas(d), i_object(obj) { }
    void Visit(PlayerMapType &m)
    {
        Player* source = nullptr;
        for (PlayerMapType::iterator iter = m.begin(); iter != m.end(); ++iter)
        {
            source = iter->GetSource();

            BuildPacket(source);

            if (!source->GetSharedVisionList().empty())
            {
                SharedVisionList::const_iterator it = source->GetSharedVisionList().begin();
                for (; it != source->GetSharedVisionList().end(); ++it)
                    BuildPacket(*it);
            }
        }
    }

    void Visit(CreatureMapType &m)
    {
        Creature* source = nullptr;
        for (CreatureMapType::iterator iter = m.begin(); iter != m.end(); ++iter)
        {
            source = iter->GetSource();
            if (!source->GetSharedVisionList().empty())
            {
                SharedVisionList::const_iterator it = source->GetSharedVisionList().begin();
                for (; it != source->GetSharedVisionList().end(); ++it)
                    BuildPacket(*it);
            }
        }
    }

    void Visit(DynamicObjectMapType &m)
    {
        DynamicObject* source = nullptr;
        for (DynamicObjectMapType::iterator iter = m.begin(); iter != m.end(); ++iter)
        {
            source = iter->GetSource();
            ObjectGuid guid = source->GetCasterGUID();

            if (guid.IsPlayer())
            {
                //Caster may be nullptr if DynObj is in removelist
                if (Player* caster = ObjectAccessor::FindPlayer(guid))
                    if (*caster->m_activePlayerData->FarsightObject == source->GetGUID())
                        BuildPacket(caster);
            }
        }
    }

    void BuildPacket(Player* player)
    {
        // Only send update once to a player
        if (plr_list.find(player->GetGUID()) == plr_list.end() && player->HaveAtClient(&i_object))
        {
            i_object.BuildFieldsUpdate(player, i_updateDatas);
            plr_list.insert(player->GetGUID());
        }
    }

    template<class SKIP> void Visit(GridRefManager<SKIP> &) { }
};

void WorldObject::BuildUpdate(UpdateDataMapType& data_map)
{
    WorldObjectChangeAccumulator notifier(*this, data_map);
    //we must build packets for all visible players
    Cell::VisitWorldObjects(this, notifier, GetVisibilityRange());

    ClearUpdateMask(false);
}

void WorldObject::AddToObjectUpdate()
{
    GetMap()->AddUpdateObject(this);
}

void WorldObject::RemoveFromObjectUpdate()
{
    GetMap()->RemoveUpdateObject(this);
}

ObjectGuid WorldObject::GetTransGUID() const
{
    if (GetTransport())
        return GetTransport()->GetGUID();
    return ObjectGuid::Empty;
}

float WorldObject::GetFloorZ() const
{
    if (!IsInWorld())
        return m_staticFloorZ;
    return std::max<float>(m_staticFloorZ, GetMap()->GetGameObjectFloor(GetPhaseShift(), GetPositionX(), GetPositionY(), GetPositionZ()));
}

template TC_GAME_API void WorldObject::GetGameObjectListWithEntryInGrid(std::list<GameObject*>&, uint32, float) const;
template TC_GAME_API void WorldObject::GetGameObjectListWithEntryInGrid(std::deque<GameObject*>&, uint32, float) const;
template TC_GAME_API void WorldObject::GetGameObjectListWithEntryInGrid(std::vector<GameObject*>&, uint32, float) const;

template TC_GAME_API void WorldObject::GetCreatureListWithEntryInGrid(std::list<Creature*>&, uint32, float) const;
template TC_GAME_API void WorldObject::GetCreatureListWithEntryInGrid(std::deque<Creature*>&, uint32, float) const;
template TC_GAME_API void WorldObject::GetCreatureListWithEntryInGrid(std::vector<Creature*>&, uint32, float) const;

template TC_GAME_API void WorldObject::GetPlayerListInGrid(std::list<Player*>&, float) const;
template TC_GAME_API void WorldObject::GetPlayerListInGrid(std::deque<Player*>&, float) const;
template TC_GAME_API void WorldObject::GetPlayerListInGrid(std::vector<Player*>&, float) const;
