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

#include "AreaTrigger.h"
#include "AreaTriggerAI.h"
#include "AreaTriggerDataStore.h"
#include "AreaTriggerPackets.h"
#include "CellImpl.h"
#include "Chat.h"
#include "Containers.h"
#include "CreatureAISelector.h"
#include "DB2Stores.h"
#include "GridNotifiersImpl.h"
#include "Language.h"
#include "Log.h"
#include "Object.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "PhasingHandler.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "Spline.h"
#include "Transport.h"
#include "Unit.h"
#include "UpdateData.h"
#include "ZoneScript.h"
#include "advstd.h"
#include <bit>

AreaTrigger::AreaTrigger() : WorldObject(false), MapObject(), _spawnId(0), _aurEff(nullptr),
    _duration(0), _totalDuration(0), _timeSinceCreated(0), _verticesUpdatePreviousOrientation(std::numeric_limits<float>::infinity()),
    _isRemoved(false), _reachedDestination(true), _lastSplineIndex(0), _movementTime(0),
    _areaTriggerCreateProperties(nullptr), _areaTriggerTemplate(nullptr)
{
    m_objectType |= TYPEMASK_AREATRIGGER;
    m_objectTypeId = TYPEID_AREATRIGGER;

    m_updateFlag = UPDATEFLAG_STATIONARY_POSITION | UPDATEFLAG_AREATRIGGER;

    m_valuesCount = AREATRIGGER_END;
    _dynamicValuesCount = AREATRIGGER_DYNAMIC_END;
}

AreaTrigger::~AreaTrigger()
{
}

void AreaTrigger::AddToWorld()
{
    ///- Register the AreaTrigger for guid lookup and for caster
    if (!IsInWorld())
    {
        if (m_zoneScript)
            m_zoneScript->OnAreaTriggerCreate(this);

        GetMap()->GetObjectsStore().Insert<AreaTrigger>(GetGUID(), this);
        if (_spawnId)
            GetMap()->GetAreaTriggerBySpawnIdStore().insert(std::make_pair(_spawnId, this));

        WorldObject::AddToWorld();
    }
}

void AreaTrigger::RemoveFromWorld()
{
    ///- Remove the AreaTrigger from the accessor and from all lists of objects in world
    if (IsInWorld())
    {
        if (m_zoneScript)
            m_zoneScript->OnAreaTriggerRemove(this);

        _isRemoved = true;

        if (Unit* caster = GetCaster())
            caster->_UnregisterAreaTrigger(this);

        _ai->OnRemove();

        // Handle removal of all units, calling OnUnitExit & deleting auras if needed
        HandleUnitEnterExit({});

        WorldObject::RemoveFromWorld();

        if (IsStaticSpawn())
            Trinity::Containers::MultimapErasePair(GetMap()->GetAreaTriggerBySpawnIdStore(), _spawnId, this);
        GetMap()->GetObjectsStore().Remove<AreaTrigger>(GetGUID());
    }
}

bool AreaTrigger::Create(AreaTriggerCreatePropertiesId areaTriggerCreatePropertiesId, Map* map, Position const& pos, int32 duration, AreaTriggerSpawn const* spawnData /* nullptr */, Unit* caster /*= nullptr*/, Unit* target /*= nullptr*/, uint32 spellXSpellVisualId /*= { 0, 0 }*/, SpellInfo const* spellInfo /*= nullptr*/, Spell* spell /*= nullptr*/, AuraEffect const* aurEff /*= nullptr*/)
{
    _targetGuid = target ? target->GetGUID() : ObjectGuid::Empty;
    _aurEff = aurEff;

    SetMap(map);
    Relocate(pos);
    RelocateStationaryPosition(pos);
    if (!IsPositionValid())
    {
        TC_LOG_ERROR("entities.areatrigger", "AreaTrigger (AreaTriggerCreatePropertiesId: (Id: {}, IsCustom: {})) not created. Invalid coordinates (X: {} Y: {})", areaTriggerCreatePropertiesId.Id, uint32(areaTriggerCreatePropertiesId.IsCustom), GetPositionX(), GetPositionY());
        return false;
    }

    _areaTriggerCreateProperties = sAreaTriggerDataStore->GetAreaTriggerCreateProperties(areaTriggerCreatePropertiesId);
    if (!_areaTriggerCreateProperties)
    {
        TC_LOG_ERROR("entities.areatrigger", "AreaTrigger (AreaTriggerCreatePropertiesId: (Id: {}, IsCustom: {})) not created. Invalid areatrigger create properties id", areaTriggerCreatePropertiesId.Id, uint32(areaTriggerCreatePropertiesId.IsCustom));
        return false;
    }

    SetZoneScript();

    _areaTriggerTemplate = _areaTriggerCreateProperties->Template;

    Object::_Create(ObjectGuid::Create<HighGuid::AreaTrigger>(GetMapId(), GetTemplate() ? GetTemplate()->Id.Id : 0, GetMap()->GenerateLowGuid<HighGuid::AreaTrigger>()));

    if (GetTemplate())
        SetEntry(GetTemplate()->Id.Id);

    SetObjectScale(1.0f);
    SetDuration(duration);

    _shape = GetCreateProperties()->Shape;

    if (caster)
        SetGuidValue(AREATRIGGER_CASTER, caster->GetGUID());
    if (spell)
        SetGuidValue(AREATRIGGER_CREATING_EFFECT_GUID, spell->m_castId);
    if (spellInfo && !IsStaticSpawn())
        SetUInt32Value(AREATRIGGER_SPELLID, spellInfo->Id);
    if (spellInfo)
        SetUInt32Value(AREATRIGGER_SPELL_FOR_VISUALS, spellInfo->Id);
    SetUInt32Value(AREATRIGGER_SPELL_X_SPELL_VISUAL_ID, spellXSpellVisualId);
    if (!IsStaticSpawn())
        SetUInt32Value(AREATRIGGER_TIME_TO_TARGET_SCALE, GetCreateProperties()->TimeToTargetScale != 0 ? GetCreateProperties()->TimeToTargetScale : GetUInt32Value(AREATRIGGER_DURATION));

    SetFloatValue(AREATRIGGER_BOUNDS_RADIUS_2D, GetCreateProperties()->Shape.GetMaxSearchRadius());
    SetUInt32Value(AREATRIGGER_DECAL_PROPERTIES_ID, GetCreateProperties()->DecalPropertiesId);

    if (IsServerSide())
        SetUInt32Value(AREATRIGGER_DECAL_PROPERTIES_ID, 24); // Blue decal, for .debug areatrigger visibility

    AreaTriggerScaleCurveTemplate const extraScaleCurve = IsStaticSpawn() ? AreaTriggerScaleCurveTemplate() : *GetCreateProperties()->ExtraScale;
    SetScaleCurve(AREATRIGGER_EXTRA_SCALE_CURVE, extraScaleCurve);


    if (caster)
    {
        if (Player const* modOwner = caster->GetSpellModOwner())
        {
            float multiplier = 1.0f;
            int32 flat = 0;
            modOwner->GetSpellModValues(spellInfo, SpellModOp::Radius, spell, GetUInt32Value(AREATRIGGER_BOUNDS_RADIUS_2D), &flat, &multiplier);
            if (multiplier != 1.0f)
            {
                AreaTriggerScaleCurveTemplate overrideScale;
                overrideScale.Curve = multiplier;
                SetScaleCurve(AREATRIGGER_OVERRIDE_SCALE_CURVE, overrideScale);
            }
        }
    }

    if (caster)
        PhasingHandler::InheritPhaseShift(this, caster);
    else if (IsStaticSpawn() && spawnData)
    {
        if (spawnData->phaseUseFlags || spawnData->phaseId || spawnData->phaseGroup)
            PhasingHandler::InitDbPhaseShift(GetPhaseShift(), spawnData->phaseUseFlags, spawnData->phaseId, spawnData->phaseGroup);
    }

    if (target && GetCreateProperties() && GetCreateProperties()->Flags.HasFlag(AreaTriggerCreatePropertiesFlag::HasAttached))
        m_movementInfo.transport.guid = target->GetGUID();

    if (!IsStaticSpawn())
        UpdatePositionData();

    UpdateShape();

    uint32 timeToTarget = GetCreateProperties()->TimeToTarget != 0 ? GetCreateProperties()->TimeToTarget : GetUInt32Value(AREATRIGGER_DURATION);

    if (GetCreateProperties()->OrbitInfo)
    {
        AreaTriggerOrbitInfo orbit = *GetCreateProperties()->OrbitInfo;
        if (target && GetCreateProperties() && GetCreateProperties()->Flags.HasFlag(AreaTriggerCreatePropertiesFlag::HasAttached))
            orbit.PathTarget = target->GetGUID();
        else
            orbit.Center = pos;

        InitOrbit(orbit, timeToTarget);
    }
    else if (GetCreateProperties()->HasSplines())
    {
        InitSplineOffsets(GetCreateProperties()->SplinePoints, timeToTarget);
    }

    // movement on transport of areatriggers on unit is handled by themself
    TransportBase* transport = nullptr;
    if (caster)
    {
        transport = m_movementInfo.transport.guid.IsEmpty() ? caster->GetTransport() : nullptr;
        if (transport)
        {
            float x, y, z, o;
            pos.GetPosition(x, y, z, o);
            transport->CalculatePassengerOffset(x, y, z, &o);
            m_movementInfo.transport.pos.Relocate(x, y, z, o);

            // This object must be added to transport before adding to map for the client to properly display it
            transport->AddPassenger(this);
        }
    }

    AI_Initialize();

    // Relocate areatriggers with circular movement again
    if (HasOrbit())
        Relocate(CalculateOrbitPosition());

    if (!IsStaticSpawn())
    {
        if (!GetMap()->AddToMap(this))
        {
            // Returning false will cause the object to be deleted - remove from transport
            if (transport)
                transport->RemovePassenger(this);
            return false;
        }
    }

    if (caster)
        caster->_RegisterAreaTrigger(this);

    _ai->OnCreate(spell ? spell : nullptr);

    return true;
}

AreaTrigger* AreaTrigger::CreateAreaTrigger(AreaTriggerCreatePropertiesId areaTriggerCreatePropertiesId, Position const& pos, int32 duration, Unit * caster, Unit * target, uint32 spellXSpellVisualID /*= { 0, 0 }*/, SpellInfo const* spellInfo /*= nullptr*/, Spell* spell /*= nullptr*/, AuraEffect const* aurEff /*= nullptr*/)
{
    AreaTrigger* at = new AreaTrigger();
    if (!at->Create(areaTriggerCreatePropertiesId, caster->GetMap(), pos, duration, nullptr, caster, target, spellXSpellVisualID, spellInfo, spell, aurEff))
    {
        delete at;
        return nullptr;
    }

    return at;
}

ObjectGuid AreaTrigger::CreateNewMovementForceId(Map* map, uint32 areaTriggerId)
{
    return ObjectGuid::Create<HighGuid::AreaTrigger>(map->GetId(), areaTriggerId, map->GenerateLowGuid<HighGuid::AreaTrigger>());
}

bool AreaTrigger::LoadFromDB(ObjectGuid::LowType spawnId, Map* map, bool /*addToMap*/, bool /*allowDuplicate*/)
{
    _spawnId = spawnId;

    AreaTriggerSpawn const* spawnData = sAreaTriggerDataStore->GetAreaTriggerSpawn(spawnId);
    if (!spawnData)
        return false;

    AreaTriggerCreateProperties const* createProperties = sAreaTriggerDataStore->GetAreaTriggerCreateProperties(spawnData->Id);
    if (!createProperties)
        return false;

    SpellInfo const* spellInfo = nullptr;
    uint32 spellXSpellVisualID = 0;
    if (spawnData->SpellForVisuals)
    {
        spellInfo = sSpellMgr->GetSpellInfo(*spawnData->SpellForVisuals, DIFFICULTY_NONE);

        if (spellInfo)
            spellXSpellVisualID = spellInfo->GetSpellXSpellVisualId();
    }

    return Create(spawnData->Id, map, spawnData->spawnPoint, -1, spawnData, nullptr, nullptr, spellXSpellVisualID, spellInfo);
}

void AreaTrigger::Update(uint32 diff)
{
    WorldObject::Update(diff);
    _timeSinceCreated += diff;

    if (!IsStaticSpawn())
    {
        if (HasOrbit())
        {
            UpdateOrbitPosition(diff);
        }
        else if (GetCreateProperties() && GetCreateProperties()->Flags.HasFlag(AreaTriggerCreatePropertiesFlag::HasAttached))
        {
            if (Unit* target = GetTarget())
            {
                float orientation = 0.0f;
                if (AreaTriggerCreateProperties const* createProperties = GetCreateProperties())
                    if (createProperties->FacingCurveId)
                        orientation = sDB2Manager.GetCurveValueAt(createProperties->FacingCurveId, GetProgress());

                if (!GetCreateProperties() || !GetCreateProperties()->Flags.HasFlag(AreaTriggerCreatePropertiesFlag::HasAbsoluteOrientation))
                    orientation += target->GetOrientation();

                GetMap()->AreaTriggerRelocation(this, target->GetPositionX(), target->GetPositionY(), target->GetPositionZ(), orientation);
            }
        }
        else if (HasSplines())
        {
            UpdateSplinePosition(diff);
        }
        else
        {
            if (AreaTriggerCreateProperties const* createProperties = GetCreateProperties())
            {
                if (createProperties->FacingCurveId)
                {
                    float orientation = sDB2Manager.GetCurveValueAt(createProperties->FacingCurveId, GetProgress());
                    if (!GetCreateProperties() || !GetCreateProperties()->Flags.HasFlag(AreaTriggerCreatePropertiesFlag::HasAbsoluteOrientation))
                        orientation += GetStationaryO();

                    SetOrientation(orientation);
                }
            }

            UpdateShape();
        }
    }

    if (GetDuration() != -1)
    {
        if (GetDuration() > int32(diff))
            _UpdateDuration(_duration - diff);
        else
        {
            Remove(); // expired
            return;
        }
    }

    _ai->OnUpdate(diff);

    UpdateTargetList();
}

void AreaTrigger::Remove()
{
    if (IsInWorld())
    {
        AddObjectToRemoveList(); // calls RemoveFromWorld
    }
}

void AreaTrigger::SetOverrideScaleCurve(float overrideScale)
{
    SetScaleCurve(AREATRIGGER_OVERRIDE_SCALE_CURVE, overrideScale);
}

void AreaTrigger::SetOverrideScaleCurve(std::array<DBCPosition2D, 2> const& points, Optional<uint32> startTimeOffset, CurveInterpolationMode interpolation)
{
    SetScaleCurve(AREATRIGGER_OVERRIDE_SCALE_CURVE, points, startTimeOffset, interpolation);
}

void AreaTrigger::ClearOverrideScaleCurve()
{
    ClearScaleCurve(AREATRIGGER_OVERRIDE_SCALE_CURVE);
}

void AreaTrigger::SetExtraScaleCurve(float extraScale)
{
    SetScaleCurve(AREATRIGGER_EXTRA_SCALE_CURVE, extraScale);
}

void AreaTrigger::SetExtraScaleCurve(std::array<DBCPosition2D, 2> const& points, Optional<uint32> startTimeOffset, CurveInterpolationMode interpolation)
{
    SetScaleCurve(AREATRIGGER_EXTRA_SCALE_CURVE, points, startTimeOffset, interpolation);
}

void AreaTrigger::ClearExtraScaleCurve()
{
    ClearScaleCurve(AREATRIGGER_EXTRA_SCALE_CURVE);
}



void AreaTrigger::SetDuration(int32 newDuration)
{
    _duration = newDuration;
    _totalDuration = newDuration;

    // negative duration (permanent areatrigger) sent as 0
    SetUInt32Value(AREATRIGGER_DURATION, std::max(newDuration, 0));
}

void AreaTrigger::_UpdateDuration(int32 newDuration)
{
    _duration = newDuration;

    // should be sent in object create packets only
    m_uint32Values[AREATRIGGER_DURATION] = _duration;
}

float AreaTrigger::CalcCurrentScale() const
{
    float scale = 1.0f;
    uint32 value = GetUInt32Value(AREATRIGGER_OVERRIDE_SCALE_CURVE + AsUnderlyingType(OVERRIDE_ACTIVE_OFFSET));
    if (value)
        scale *= std::max(GetScaleCurveValue(AREATRIGGER_OVERRIDE_SCALE_CURVE, GetUInt32Value(AREATRIGGER_TIME_TO_TARGET_SCALE)), 0.000001f);
    else if (AreaTriggerCreateProperties const* createProperties = GetCreateProperties())
        if (createProperties->ScaleCurveId)
            scale *= std::max(sDB2Manager.GetCurveValueAt(createProperties->ScaleCurveId, GetScaleCurveProgress(AREATRIGGER_OVERRIDE_SCALE_CURVE, GetUInt32Value(AREATRIGGER_TIME_TO_TARGET_SCALE))), 0.000001f);

    scale *= std::max(GetScaleCurveValue(AREATRIGGER_EXTRA_SCALE_CURVE, GetUInt32Value(AREATRIGGER_TIME_TO_TARGET_EXTRA_SCALE)), 0.000001f);

    return scale;
}

float AreaTrigger::GetProgress() const
{
    if (_totalDuration <= 0)
        return 1.0f;

    return std::clamp(float(GetTimeSinceCreated()) / float(GetTotalDuration()), 0.0f, 1.0f);
}

float AreaTrigger::GetScaleCurveProgress(uint16 scaleCurveAt, uint32 timeTo) const
{
    if (!timeTo)
        return 0.0f;

    return std::clamp(float(GetTimeSinceCreated() - GetUInt32Value(scaleCurveAt + START_TIME_OFFSET)) / float(timeTo), 0.0f, 1.0f);
}

float AreaTrigger::GetScaleCurveValueAtProgress(uint16 scaleCurveAt, float x) const
{
    ASSERT(GetUInt32Value(scaleCurveAt + OVERRIDE_ACTIVE_OFFSET), "ScaleCurve must be active to evaluate it");

    // unpack ParameterCurve
    uint32 parameterCurve = GetUInt32Value(scaleCurveAt + PARAMETER_CURVE_OFFSET);
    if (parameterCurve & 1)
        return std::bit_cast<float>(parameterCurve & ~1);

    std::array<DBCPosition2D, 2> points;

    points[0] = { .X = GetFloatValue(scaleCurveAt + POINT1_X_OFFSET), .Y = GetFloatValue(scaleCurveAt + POINT1_Y_OFFSET)};
    points[1] = { .X = GetFloatValue(scaleCurveAt + POINT2_Y_OFFSET), .Y = GetFloatValue(scaleCurveAt + POINT2_Y_OFFSET)};


    CurveInterpolationMode mode = CurveInterpolationMode(parameterCurve >> 1 & 0x7);
    std::size_t pointCount = parameterCurve >> 24 & 0xFF;

    return sDB2Manager.GetCurveValueAt(mode, std::span(points.begin(), pointCount), x);
}

float AreaTrigger::GetScaleCurveValue(uint16 scaleCurveAt, uint32 timeTo) const
{
    return GetScaleCurveValueAtProgress(scaleCurveAt, GetScaleCurveProgress(scaleCurveAt, timeTo));
}

void AreaTrigger::SetScaleCurve(uint16 scaleCurveAt, float constantValue)
{
    AreaTriggerScaleCurveTemplate curveTemplate;
    curveTemplate.Curve = constantValue;
    SetScaleCurve(scaleCurveAt, curveTemplate);
}

void AreaTrigger::SetScaleCurve(uint16 scaleCurveAt, std::array<DBCPosition2D, 2> const& points,
    Optional<uint32> startTimeOffset, CurveInterpolationMode interpolation)
{
    AreaTriggerScaleCurvePointsTemplate curve;
    curve.Mode = interpolation;
    curve.Points = points;

    AreaTriggerScaleCurveTemplate curveTemplate;
    curveTemplate.StartTimeOffset = startTimeOffset.value_or(GetTimeSinceCreated());
    curveTemplate.Curve = curve;

    SetScaleCurve(scaleCurveAt, curveTemplate);
}

void AreaTrigger::ClearScaleCurve(uint16 scaleCurveAt)
{
    SetUInt32Value(scaleCurveAt + START_TIME_OFFSET, 0);
    SetUInt32Value(scaleCurveAt + PARAMETER_CURVE_OFFSET, 0);
    SetFloatValue(scaleCurveAt + POINT1_X_OFFSET, 0);
    SetFloatValue(scaleCurveAt + POINT1_Y_OFFSET, 0);
    SetFloatValue(scaleCurveAt + POINT2_X_OFFSET, 0);
    SetFloatValue(scaleCurveAt + POINT2_Y_OFFSET, 0);
    SetUInt32Value(scaleCurveAt + OVERRIDE_ACTIVE_OFFSET, false);
}

void AreaTrigger::SetScaleCurve(uint16 scaleCurveAt, Optional<AreaTriggerScaleCurveTemplate> const& curve)
{
    if (!curve)
    {
        SetUInt32Value(scaleCurveAt + OVERRIDE_ACTIVE_OFFSET, false);
        return;
    }

    SetUInt32Value(scaleCurveAt + OVERRIDE_ACTIVE_OFFSET, true);
    SetUInt32Value(scaleCurveAt + START_TIME_OFFSET, curve->StartTimeOffset);

    Position point;
    // ParameterCurve packing information
    // (not_using_points & 1) | ((interpolation_mode & 0x7) << 1) | ((first_point_offset & 0xFFFFF) << 4) | ((point_count & 0xFF) << 24)
    //   if not_using_points is set then the entire field is simply read as a float (ignoring that lowest bit)

    if (float const* simpleFloat = std::get_if<float>(&curve->Curve))
    {
        uint32 packedCurve = std::bit_cast<uint32>(*simpleFloat);
        packedCurve |= 1;

        SetUInt32Value(scaleCurveAt + PARAMETER_CURVE_OFFSET, packedCurve);
        SetFloatValue(scaleCurveAt + POINT1_X_OFFSET, 0);
        SetFloatValue(scaleCurveAt + POINT1_Y_OFFSET, 0);
        SetFloatValue(scaleCurveAt + POINT2_X_OFFSET, 0);
        SetFloatValue(scaleCurveAt + POINT2_Y_OFFSET, 0);
    }
    else if (AreaTriggerScaleCurvePointsTemplate const* curvePoints = std::get_if<AreaTriggerScaleCurvePointsTemplate>(&curve->Curve))
    {
        CurveInterpolationMode mode = curvePoints->Mode;
        if (curvePoints->Points[1].X < curvePoints->Points[0].X)
            mode = CurveInterpolationMode::Constant;

        switch (mode)
        {
            case CurveInterpolationMode::CatmullRom:
                // catmullrom requires at least 4 points, impossible here
                mode = CurveInterpolationMode::Cosine;
                break;
            case CurveInterpolationMode::Bezier3:
            case CurveInterpolationMode::Bezier4:
            case CurveInterpolationMode::Bezier:
                // bezier requires more than 2 points, impossible here
                mode = CurveInterpolationMode::Linear;
                break;
            default:
                break;
        }

        uint32 pointCount = 2;
        if (mode == CurveInterpolationMode::Constant)
            pointCount = 1;

        uint32 packedCurve = (uint32(mode) << 1) | (pointCount << 24);
        SetUInt32Value(scaleCurveAt + PARAMETER_CURVE_OFFSET, packedCurve);

        for (std::size_t i = 0; i < curvePoints->Points.size(); ++i)
        {
            point.Relocate(curvePoints->Points[i].X, curvePoints->Points[i].Y);
            SetFloatValue(scaleCurveAt + POINT1_X_OFFSET + i * 2, point.GetPositionX());
            SetFloatValue(scaleCurveAt + POINT1_Y_OFFSET + i * 2, point.GetPositionY());
        }
    }
}

void AreaTrigger::UpdateTargetList()
{
    std::vector<Unit*> targetList;

    switch (_shape.Type)
    {
        case AreaTriggerShapeType::Sphere:
            SearchUnitInSphere(targetList);
            break;
        case AreaTriggerShapeType::Box:
            SearchUnitInBox(targetList);
            break;
        case AreaTriggerShapeType::Polygon:
            SearchUnitInPolygon(targetList);
            break;
        case AreaTriggerShapeType::Cylinder:
            SearchUnitInCylinder(targetList);
            break;
        case AreaTriggerShapeType::Disk:
            SearchUnitInDisk(targetList);
            break;
        case AreaTriggerShapeType::BoundedPlane:
            SearchUnitInBoundedPlane(targetList);
            break;
        default:
            break;
    }

    if (GetTemplate())
    {
        if (ConditionContainer const* conditions = sConditionMgr->GetConditionsForAreaTrigger(GetTemplate()->Id.Id, GetTemplate()->Id.IsCustom))
        {
            Trinity::Containers::EraseIf(targetList, [conditions](Unit const* target)
            {
                return !sConditionMgr->IsObjectMeetToConditions(target, *conditions);
            });
        }
    }

    HandleUnitEnterExit(targetList);
}

void AreaTrigger::SearchUnits(std::vector<Unit*>& targetList, float radius, bool check3D)
{
    Trinity::AnyUnitInObjectRangeCheck check(this, radius, check3D);
    if (IsStaticSpawn())
    {
        Trinity::PlayerListSearcher<Trinity::AnyUnitInObjectRangeCheck> searcher(this, targetList, check);
        Cell::VisitWorldObjects(this, searcher, GetMaxSearchRadius());
    }
    else
    {
        Trinity::UnitListSearcher<Trinity::AnyUnitInObjectRangeCheck> searcher(this, targetList, check);
        Cell::VisitAllObjects(this, searcher, GetMaxSearchRadius());
    }
}

void AreaTrigger::SearchUnitInSphere(std::vector<Unit*>& targetList)
{
    float progress = GetProgress();
    if (AreaTriggerCreateProperties const* createProperties = GetCreateProperties())
        if (createProperties->MorphCurveId)
            progress = sDB2Manager.GetCurveValueAt(createProperties->MorphCurveId, progress);

    float scale = CalcCurrentScale();
    float radius = G3D::lerp(_shape.SphereDatas.Radius, _shape.SphereDatas.RadiusTarget, progress) * scale;

    SearchUnits(targetList, radius, true);
}

void AreaTrigger::SearchUnitInBox(std::vector<Unit*>& targetList)
{
    float progress = GetProgress();
    if (AreaTriggerCreateProperties const* createProperties = GetCreateProperties())
        if (createProperties->MorphCurveId)
            progress = sDB2Manager.GetCurveValueAt(createProperties->MorphCurveId, progress);

    float scale = CalcCurrentScale();
    float extentsX = G3D::lerp(_shape.BoxDatas.Extents[0], _shape.BoxDatas.ExtentsTarget[0], progress) * scale;
    float extentsY = G3D::lerp(_shape.BoxDatas.Extents[1], _shape.BoxDatas.ExtentsTarget[1], progress) * scale;
    float extentsZ = G3D::lerp(_shape.BoxDatas.Extents[2], _shape.BoxDatas.ExtentsTarget[2], progress) * scale;
    float radius = std::sqrt(extentsX * extentsX + extentsY * extentsY);

    SearchUnits(targetList, radius, false);

    Position const& boxCenter = GetPosition();
    Trinity::Containers::EraseIf(targetList, [boxCenter, extentsX, extentsY, extentsZ](Unit const* unit) -> bool
    {
        return !unit->IsWithinBox(boxCenter, extentsX, extentsY, extentsZ / 2);
    });
}

void AreaTrigger::SearchUnitInPolygon(std::vector<Unit*>& targetList)
{
    float progress = GetProgress();
    if (AreaTriggerCreateProperties const* createProperties = GetCreateProperties())
        if (createProperties->MorphCurveId)
            progress = sDB2Manager.GetCurveValueAt(createProperties->MorphCurveId, progress);

    float height = G3D::lerp(_shape.PolygonDatas.Height, _shape.PolygonDatas.HeightTarget, progress);
    float minZ = GetPositionZ() - height;
    float maxZ = GetPositionZ() + height;

    SearchUnits(targetList, GetMaxSearchRadius(), false);

    Trinity::Containers::EraseIf(targetList, [this, minZ, maxZ](Unit const* unit) -> bool
    {
        return unit->GetPositionZ() < minZ
            || unit->GetPositionZ() > maxZ
            || !CheckIsInPolygon2D(unit);
    });
}

void AreaTrigger::SearchUnitInCylinder(std::vector<Unit*>& targetList)
{
    float progress = GetProgress();
    if (AreaTriggerCreateProperties const* createProperties = GetCreateProperties())
        if (createProperties->MorphCurveId)
            progress = sDB2Manager.GetCurveValueAt(createProperties->MorphCurveId, progress);

    float scale = CalcCurrentScale();
    float radius = G3D::lerp(_shape.CylinderDatas.Radius, _shape.CylinderDatas.RadiusTarget, progress) * scale;
    float height = G3D::lerp(_shape.CylinderDatas.Height, _shape.CylinderDatas.HeightTarget, progress);

    float minZ = GetPositionZ() - height;
    float maxZ = GetPositionZ() + height;

    SearchUnits(targetList, radius, false);

    Trinity::Containers::EraseIf(targetList, [minZ, maxZ](Unit const* unit) -> bool
    {
        return unit->GetPositionZ() < minZ
            || unit->GetPositionZ() > maxZ;
    });
}

void AreaTrigger::SearchUnitInDisk(std::vector<Unit*>& targetList)
{
    float progress = GetProgress();
    if (AreaTriggerCreateProperties const* createProperties = GetCreateProperties())
        if (createProperties->MorphCurveId)
            progress = sDB2Manager.GetCurveValueAt(createProperties->MorphCurveId, progress);

    float scale = CalcCurrentScale();
    float innerRadius = G3D::lerp(_shape.DiskDatas.InnerRadius, _shape.DiskDatas.InnerRadiusTarget, progress) * scale;
    float outerRadius = G3D::lerp(_shape.DiskDatas.OuterRadius, _shape.DiskDatas.OuterRadiusTarget, progress) * scale;
    float height = G3D::lerp(_shape.DiskDatas.Height, _shape.DiskDatas.HeightTarget, progress);

    float minZ = GetPositionZ() - height;
    float maxZ = GetPositionZ() + height;

    SearchUnits(targetList, outerRadius, false);

    Trinity::Containers::EraseIf(targetList, [this, innerRadius, minZ, maxZ](Unit const* unit) -> bool
    {
        return unit->IsInDist2d(this, innerRadius) || unit->GetPositionZ() < minZ || unit->GetPositionZ() > maxZ;
    });
}

void AreaTrigger::SearchUnitInBoundedPlane(std::vector<Unit*>& targetList)
{
    float progress = GetProgress();
    if (AreaTriggerCreateProperties const* createProperties = GetCreateProperties())
        if (createProperties->MorphCurveId)
            progress = sDB2Manager.GetCurveValueAt(createProperties->MorphCurveId, progress);

    float scale = CalcCurrentScale();
    float extentsX = G3D::lerp(_shape.BoundedPlaneDatas.Extents[0], _shape.BoundedPlaneDatas.ExtentsTarget[0], progress) * scale;
    float extentsY = G3D::lerp(_shape.BoundedPlaneDatas.Extents[1], _shape.BoundedPlaneDatas.ExtentsTarget[1], progress) * scale;
    float radius = std::sqrt(extentsX * extentsX + extentsY * extentsY);

    SearchUnits(targetList, radius, false);

    Position const& boxCenter = GetPosition();
    Trinity::Containers::EraseIf(targetList, [boxCenter, extentsX, extentsY](Unit const* unit) -> bool
    {
        return !unit->IsWithinBox(boxCenter, extentsX, extentsY, MAP_SIZE);
    });
}

void AreaTrigger::HandleUnitEnterExit(std::vector<Unit*> const& newTargetList)
{
    GuidUnorderedSet exitUnits(std::move(_insideUnits));

    std::vector<Unit*> enteringUnits;

    for (Unit* unit : newTargetList)
    {
        if (exitUnits.erase(unit->GetGUID()) == 0) // erase(key_type) returns number of elements erased
            enteringUnits.push_back(unit);

        _insideUnits.insert(unit->GetGUID());
    }

    // Handle after _insideUnits have been reinserted so we can use GetInsideUnits() in hooks
    for (Unit* unit : enteringUnits)
    {
        if (Player* player = unit->ToPlayer())
        {
            if (player->isDebugAreaTriggers)
                ChatHandler(player->GetSession()).PSendSysMessage(LANG_DEBUG_AREATRIGGER_ENTITY_ENTERED, GetEntry(), IsCustom(), IsStaticSpawn(), GetGUID().GetCounter());

            player->UpdateQuestObjectiveProgress(QUEST_OBJECTIVE_AREA_TRIGGER_ENTER, GetEntry(), 1);
        }

        DoActions(unit);

        _ai->OnUnitEnter(unit);
    }

    for (ObjectGuid const& exitUnitGuid : exitUnits)
    {
        if (Unit* leavingUnit = ObjectAccessor::GetUnit(*this, exitUnitGuid))
        {
            if (Player* player = leavingUnit->ToPlayer())
            {
                if (player->isDebugAreaTriggers)
                    ChatHandler(player->GetSession()).PSendSysMessage(LANG_DEBUG_AREATRIGGER_ENTITY_LEFT, GetEntry(), IsCustom(), IsStaticSpawn(), GetGUID().GetCounter());

                player->UpdateQuestObjectiveProgress(QUEST_OBJECTIVE_AREA_TRIGGER_EXIT, GetEntry(), 1);
            }

            UndoActions(leavingUnit);

            _ai->OnUnitExit(leavingUnit);
        }
    }
}

AreaTriggerTemplate const* AreaTrigger::GetTemplate() const
{
    return _areaTriggerTemplate;
}

uint32 AreaTrigger::GetScriptId() const
{
    if (_spawnId)
        return ASSERT_NOTNULL(sAreaTriggerDataStore->GetAreaTriggerSpawn(_spawnId))->scriptId;

    if (AreaTriggerCreateProperties const* createProperties = GetCreateProperties())
        return createProperties->ScriptId;

    return 0;
}

Unit* AreaTrigger::GetCaster() const
{
    return ObjectAccessor::GetUnit(*this, GetCasterGuid());
}

Unit* AreaTrigger::GetTarget() const
{
    return ObjectAccessor::GetUnit(*this, _targetGuid);
}

uint32 AreaTrigger::GetFaction() const
{
    if (Unit const* caster = GetCaster())
        return caster->GetFaction();

    return 0;
}

float AreaTrigger::GetMaxSearchRadius() const
{
    return GetUInt32Value(AREATRIGGER_BOUNDS_RADIUS_2D) * CalcCurrentScale();
}

void AreaTrigger::UpdatePolygonVertices()
{
    AreaTriggerCreateProperties const* createProperties = GetCreateProperties();
    AreaTriggerShapeInfo const& shape = GetShape();
    float newOrientation = GetOrientation();

    // No need to recalculate, orientation didn't change
    if (G3D::fuzzyEq(_verticesUpdatePreviousOrientation, newOrientation) && shape.PolygonVerticesTarget.empty())
        return;

    _polygonVertices.assign(shape.PolygonVertices.begin(), shape.PolygonVertices.end());
    if (!shape.PolygonVerticesTarget.empty())
    {
        float progress = GetProgress();
        if (createProperties->MorphCurveId)
            progress = sDB2Manager.GetCurveValueAt(createProperties->MorphCurveId, progress);

        for (std::size_t i = 0; i < _polygonVertices.size(); ++i)
        {
            Position& vertex = _polygonVertices[i];
            Position const& vertexTarget = shape.PolygonVerticesTarget[i].Pos;

            vertex.m_positionX = G3D::lerp(vertex.GetPositionX(), vertexTarget.GetPositionX(), progress);
            vertex.m_positionY = G3D::lerp(vertex.GetPositionY(), vertexTarget.GetPositionY(), progress);
        }
    }

    float angleSin = std::sin(newOrientation);
    float angleCos = std::cos(newOrientation);

    // This is needed to rotate the vertices, following orientation
    for (Position& vertice : _polygonVertices)
    {
        float x = vertice.GetPositionX() * angleCos - vertice.GetPositionY() * angleSin;
        float y = vertice.GetPositionY() * angleCos + vertice.GetPositionX() * angleSin;
        vertice.Relocate(x, y);
    }

    _verticesUpdatePreviousOrientation = newOrientation;
}

bool AreaTrigger::CheckIsInPolygon2D(Position const* pos) const
{
    float testX = pos->GetPositionX();
    float testY = pos->GetPositionY();

    //this method uses the ray tracing algorithm to determine if the point is in the polygon
    bool locatedInPolygon = false;

    for (std::size_t vertex = 0; vertex < _polygonVertices.size(); ++vertex)
    {
        std::size_t nextVertex;

        //repeat loop for all sets of points
        if (vertex == (_polygonVertices.size() - 1))
        {
            //if i is the last vertex, let j be the first vertex
            nextVertex = 0;
        }
        else
        {
            //for all-else, let j=(i+1)th vertex
            nextVertex = vertex + 1;
        }

        float vertX_i = GetPositionX() + _polygonVertices[vertex].GetPositionX();
        float vertY_i = GetPositionY() + _polygonVertices[vertex].GetPositionY();
        float vertX_j = GetPositionX() + _polygonVertices[nextVertex].GetPositionX();
        float vertY_j = GetPositionY() + _polygonVertices[nextVertex].GetPositionY();

        // following statement checks if testPoint.Y is below Y-coord of i-th vertex
        bool belowLowY = vertY_i > testY;
        // following statement checks if testPoint.Y is below Y-coord of i+1-th vertex
        bool belowHighY = vertY_j > testY;

        /* following statement is true if testPoint.Y satisfies either (only one is possible)
        -->(i).Y < testPoint.Y < (i+1).Y        OR
        -->(i).Y > testPoint.Y > (i+1).Y

        (Note)
        Both of the conditions indicate that a point is located within the edges of the Y-th coordinate
        of the (i)-th and the (i+1)- th vertices of the polygon. If neither of the above
        conditions is satisfied, then it is assured that a semi-infinite horizontal line draw
        to the right from the testpoint will NOT cross the line that connects vertices i and i+1
        of the polygon
        */
        bool withinYsEdges = belowLowY != belowHighY;

        if (withinYsEdges)
        {
            // this is the slope of the line that connects vertices i and i+1 of the polygon
            float slopeOfLine = (vertX_j - vertX_i) / (vertY_j - vertY_i);

            // this looks up the x-coord of a point lying on the above line, given its y-coord
            float pointOnLine = (slopeOfLine* (testY - vertY_i)) + vertX_i;

            //checks to see if x-coord of testPoint is smaller than the point on the line with the same y-coord
            bool isLeftToLine = testX < pointOnLine;

            if (isLeftToLine)
            {
                //this statement changes true to false (and vice-versa)
                locatedInPolygon = !locatedInPolygon;
            }//end if (isLeftToLine)
        }//end if (withinYsEdges
    }

    return locatedInPolygon;
}

void AreaTrigger::UpdateShape()
{
    if (_shape.IsPolygon())
        UpdatePolygonVertices();
}

bool UnitFitToActionRequirement(Unit* unit, Unit* caster, AreaTriggerAction const& action)
{
    switch (action.TargetType)
    {
        case AREATRIGGER_ACTION_USER_FRIEND:
        {
            return caster->IsValidAssistTarget(unit, sSpellMgr->GetSpellInfo(action.Param, caster->GetMap()->GetDifficultyID()));
        }
        case AREATRIGGER_ACTION_USER_ENEMY:
        {
            return caster->IsValidAttackTarget(unit, sSpellMgr->GetSpellInfo(action.Param, caster->GetMap()->GetDifficultyID()));
        }
        case AREATRIGGER_ACTION_USER_RAID:
        {
            return caster->IsInRaidWith(unit);
        }
        case AREATRIGGER_ACTION_USER_PARTY:
        {
            return caster->IsInPartyWith(unit);
        }
        case AREATRIGGER_ACTION_USER_CASTER:
        {
            return unit->GetGUID() == caster->GetGUID();
        }
        case AREATRIGGER_ACTION_USER_ANY:
        default:
            break;
    }

    return true;
}

void AreaTrigger::DoActions(Unit* unit)
{
    Unit* caster = IsStaticSpawn() ? unit : GetCaster();

    if (caster && GetTemplate())
    {
        for (AreaTriggerAction const& action : GetTemplate()->Actions)
        {
            if (IsStaticSpawn() || UnitFitToActionRequirement(unit, caster, action))
            {
                switch (action.ActionType)
                {
                    case AREATRIGGER_ACTION_CAST:
                        caster->CastSpell(unit, action.Param, CastSpellExtraArgs(TRIGGERED_FULL_MASK)
                            .SetOriginalCastId(GetGuidValue(AREATRIGGER_CREATING_EFFECT_GUID).IsCast() ? GetGuidValue(AREATRIGGER_CREATING_EFFECT_GUID) : ObjectGuid::Empty));
                        break;
                    case AREATRIGGER_ACTION_ADDAURA:
                        caster->AddAura(action.Param, unit);
                        break;
                    case AREATRIGGER_ACTION_TELEPORT:
                    {
                        if (WorldSafeLocsEntry const* safeLoc = sDB2Manager.GetWorldSafeLoc(action.Param))
                        {
                            if (Player* player = caster->ToPlayer())
                            {
                                if (player->GetMapId() != safeLoc->MapID)
                                {
                                    if (WorldSafeLocsEntry const* instanceEntrance = player->GetInstanceEntrance(safeLoc->MapID))
                                        safeLoc = instanceEntrance;
                                }
                                WorldLocation loc(safeLoc->MapID, safeLoc->GetPositionX(), safeLoc->GetPositionY(), safeLoc->GetPositionZ(), safeLoc->GetOrientation());
                                player->TeleportTo(loc);
                            }
                        }
                        break;
                    }
                    default:
                        break;
                }
            }
        }
    }
}

void AreaTrigger::UndoActions(Unit* unit)
{
    if (GetTemplate())
        for (AreaTriggerAction const& action : GetTemplate()->Actions)
            if (action.ActionType == AREATRIGGER_ACTION_CAST || action.ActionType == AREATRIGGER_ACTION_ADDAURA)
                unit->RemoveAurasDueToSpell(action.Param, GetCasterGuid());
}

void AreaTrigger::InitSplineOffsets(std::vector<Position> const& offsets, uint32 timeToTarget)
{
    float angleSin = std::sin(GetOrientation());
    float angleCos = std::cos(GetOrientation());

    // This is needed to rotate the spline, following caster orientation
    std::vector<G3D::Vector3> rotatedPoints;
    rotatedPoints.reserve(offsets.size());
    for (Position const& offset : offsets)
    {
        float x = GetPositionX() + (offset.GetPositionX() * angleCos - offset.GetPositionY() * angleSin);
        float y = GetPositionY() + (offset.GetPositionY() * angleCos + offset.GetPositionX() * angleSin);
        float z = GetPositionZ();

        UpdateAllowedPositionZ(x, y, z);
        z += offset.GetPositionZ();

        rotatedPoints.emplace_back(x, y, z);
    }

    InitSplines(std::move(rotatedPoints), timeToTarget);
}

void AreaTrigger::InitSplines(std::vector<G3D::Vector3> splinePoints, uint32 timeToTarget)
{
    if (splinePoints.size() < 2)
        return;

    _movementTime = 0;

    _spline = std::make_unique<::Movement::Spline<int32>>();
    _spline->init_spline(&splinePoints[0], splinePoints.size(), ::Movement::SplineBase::ModeLinear);
    _spline->initLengths();

    // should be sent in object create packets only
    m_uint32Values[AREATRIGGER_TIME_TO_TARGET] = timeToTarget;

    if (IsInWorld())
    {
        if (_reachedDestination)
        {
            WorldPackets::AreaTrigger::AreaTriggerReShape reshape;
            reshape.TriggerGUID = GetGUID();
            SendMessageToSet(reshape.Write(), true);
        }

        WorldPackets::AreaTrigger::AreaTriggerReShape reshape;
        reshape.TriggerGUID = GetGUID();
        reshape.AreaTriggerSpline.emplace();
        reshape.AreaTriggerSpline->ElapsedTimeForMovement = GetElapsedTimeForMovement();
        reshape.AreaTriggerSpline->TimeToTarget = timeToTarget;
        for (G3D::Vector3 const& vec : splinePoints)
            reshape.AreaTriggerSpline->Points.emplace_back(vec.x, vec.y, vec.z);

        SendMessageToSet(reshape.Write(), true);
    }

    _reachedDestination = false;
}

bool AreaTrigger::HasSplines() const
{
    return bool(_spline);
}

void AreaTrigger::InitOrbit(AreaTriggerOrbitInfo const& orbit, uint32 timeToTarget)
{
    // Circular movement requires either a center position or an attached unit
    ASSERT(orbit.Center.has_value() || orbit.PathTarget.has_value());

    // should be sent in object create packets only
    m_uint32Values[AREATRIGGER_TIME_TO_TARGET] = timeToTarget;

    _orbitInfo = orbit;

    _orbitInfo->TimeToTarget = timeToTarget;
    _orbitInfo->ElapsedTimeForMovement = 0;

    if (IsInWorld())
    {
        WorldPackets::AreaTrigger::AreaTriggerReShape reshape;
        reshape.TriggerGUID = GetGUID();
        reshape.AreaTriggerOrbit = _orbitInfo;

        SendMessageToSet(reshape.Write(), true);
    }
}

bool AreaTrigger::HasOrbit() const
{
    return _orbitInfo.has_value();
}

Position const* AreaTrigger::GetOrbitCenterPosition() const
{
    if (!_orbitInfo)
        return nullptr;

    if (_orbitInfo->PathTarget)
        if (WorldObject* center = ObjectAccessor::GetWorldObject(*this, *_orbitInfo->PathTarget))
            return center;

    if (_orbitInfo->Center)
        return &_orbitInfo->Center->Pos;

    return nullptr;
}

Position AreaTrigger::CalculateOrbitPosition() const
{
    Position const* centerPos = GetOrbitCenterPosition();
    if (!centerPos)
        return GetPosition();

    AreaTriggerCreateProperties const* createProperties = GetCreateProperties();
    AreaTriggerOrbitInfo const& cmi = *_orbitInfo;

    // AreaTrigger make exactly "Duration / TimeToTarget" loops during his life time
    float pathProgress = float(cmi.ElapsedTimeForMovement) / float(cmi.TimeToTarget);
    if (createProperties && createProperties->MoveCurveId)
        pathProgress = sDB2Manager.GetCurveValueAt(createProperties->MoveCurveId, pathProgress);

    // We already made one circle and can't loop
    if (!cmi.CanLoop)
        pathProgress = std::min(1.f, pathProgress);

    float radius = cmi.Radius;
    if (G3D::fuzzyNe(cmi.BlendFromRadius, radius))
    {
        float blendCurve = (cmi.BlendFromRadius - radius) / radius;
        // 4.f Defines four quarters
        blendCurve = RoundToInterval(blendCurve, 1.f, 4.f) / 4.f;
        float blendProgress = std::min(1.f, pathProgress / blendCurve);
        radius = G3D::lerp(cmi.BlendFromRadius, cmi.Radius, blendProgress);
    }

    // Adapt Path progress depending of circle direction
    if (!cmi.CounterClockwise)
        pathProgress *= -1;

    float angle = cmi.InitialAngle + 2.f * float(M_PI) * pathProgress;
    float x = centerPos->GetPositionX() + (radius * std::cos(angle));
    float y = centerPos->GetPositionY() + (radius * std::sin(angle));
    float z = centerPos->GetPositionZ() + cmi.ZOffset;

    float orientation = 0.0f;
    if (createProperties && createProperties->FacingCurveId)
        orientation = sDB2Manager.GetCurveValueAt(createProperties->FacingCurveId, GetProgress());

    if (!GetCreateProperties() || !GetCreateProperties()->Flags.HasFlag(AreaTriggerCreatePropertiesFlag::HasAbsoluteOrientation))
    {
        orientation += angle;
        orientation += cmi.CounterClockwise ? float(M_PI_4) : -float(M_PI_4);
    }

    return { x, y, z, orientation };
}

void AreaTrigger::UpdateOrbitPosition(uint32 /*diff*/)
{
    if (_orbitInfo->StartDelay > GetElapsedTimeForMovement())
        return;

    _orbitInfo->ElapsedTimeForMovement = GetElapsedTimeForMovement() - _orbitInfo->StartDelay;

    Position pos = CalculateOrbitPosition();

    GetMap()->AreaTriggerRelocation(this, pos.GetPositionX(), pos.GetPositionY(), pos.GetPositionZ(), pos.GetOrientation());
#ifdef TRINITY_DEBUG
    DebugVisualizePosition();
#endif
}

void AreaTrigger::UpdateSplinePosition(uint32 diff)
{
    if (_reachedDestination)
        return;

    _movementTime += diff;

    if (_movementTime >= GetTimeToTarget())
    {
        _reachedDestination = true;
        _lastSplineIndex = int32(_spline->last());

        G3D::Vector3 lastSplinePosition = _spline->getPoint(_lastSplineIndex);
        GetMap()->AreaTriggerRelocation(this, lastSplinePosition.x, lastSplinePosition.y, lastSplinePosition.z, GetOrientation());
#ifdef TRINITY_DEBUG
        DebugVisualizePosition();
#endif

        _ai->OnSplineIndexReached(_lastSplineIndex);
        _ai->OnDestinationReached();
        return;
    }

    float currentTimePercent = float(_movementTime) / float(GetTimeToTarget());

    if (currentTimePercent <= 0.f)
        return;

    AreaTriggerCreateProperties const* createProperties = GetCreateProperties();
    if (createProperties && createProperties->MoveCurveId)
    {
        float progress = sDB2Manager.GetCurveValueAt(createProperties->MoveCurveId, currentTimePercent);
        if (progress < 0.f || progress > 1.f)
        {
            TC_LOG_ERROR("entities.areatrigger", "AreaTrigger (Id: {}, AreaTriggerCreatePropertiesId: (Id: {}, IsCustom: {})) has wrong progress ({}) caused by curve calculation (MoveCurveId: {})",
                GetEntry(), createProperties->Id.Id, uint32(createProperties->Id.IsCustom), progress, createProperties->MoveCurveId);
        }
        else
            currentTimePercent = progress;
    }

    int lastPositionIndex = 0;
    float percentFromLastPoint = 0;
    _spline->computeIndex(currentTimePercent, lastPositionIndex, percentFromLastPoint);

    G3D::Vector3 currentPosition;
    _spline->evaluate_percent(lastPositionIndex, percentFromLastPoint, currentPosition);

    float orientation = GetStationaryO();
    if (createProperties && createProperties->FacingCurveId)
        orientation += sDB2Manager.GetCurveValueAt(createProperties->FacingCurveId, GetProgress());

    if (GetCreateProperties() && !GetCreateProperties()->Flags.HasFlag(AreaTriggerCreatePropertiesFlag::HasAbsoluteOrientation) && GetCreateProperties()->Flags.HasFlag(AreaTriggerCreatePropertiesFlag::HasFaceMovementDir))
    {
        G3D::Vector3 derivative;
        _spline->evaluate_derivative(lastPositionIndex, percentFromLastPoint, derivative);
        if (derivative.x != 0.0f || derivative.y != 0.0f)
            orientation += std::atan2(derivative.y, derivative.x);
    }

    GetMap()->AreaTriggerRelocation(this, currentPosition.x, currentPosition.y, currentPosition.z, orientation);
#ifdef TRINITY_DEBUG
    DebugVisualizePosition();
#endif

    if (_lastSplineIndex != lastPositionIndex)
    {
        _lastSplineIndex = lastPositionIndex;
        _ai->OnSplineIndexReached(_lastSplineIndex);
    }
}

void AreaTrigger::DebugVisualizePosition()
{
    if (Unit* caster = GetCaster())
        if (Player* player = caster->ToPlayer())
            if (player->isDebugAreaTriggers)
                player->SummonCreature(1, *this, TEMPSUMMON_TIMED_DESPAWN, Milliseconds(GetTimeToTarget()));
}

void AreaTrigger::AI_Initialize()
{
    AI_Destroy();
    _ai.reset(FactorySelector::SelectAreaTriggerAI(this));
    _ai->OnInitialize();
}

void AreaTrigger::AI_Destroy()
{
    _ai.reset();
}

bool AreaTrigger::IsNeverVisibleFor(WorldObject const* seer, bool allowServersideObjects) const
{
    if (WorldObject::IsNeverVisibleFor(seer, allowServersideObjects))
        return true;

    if (IsServerSide() && !allowServersideObjects)
    {
        if (Player const* seerPlayer = seer->ToPlayer())
            return !seerPlayer->isDebugAreaTriggers;

        return true;
    }

    return false;
}
