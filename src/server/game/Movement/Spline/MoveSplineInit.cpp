/*
 * Copyright (C) 2005-2011 MaNGOS <http://getmangos.com/>
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

#include "MoveSplineInit.h"
#include "MoveSpline.h"
#include "MovementPacketBuilder.h"
#include "Unit.h"
#include "Transport.h"
#include "Vehicle.h"

namespace Movement
{
    UnitMoveType SelectSpeedType(uint32 moveFlags)
    {
        /*! Not sure about MOVEMENTFLAG_CAN_FLY here - do creatures that can fly
            but are on ground right now also have it? If yes, this needs a more
            dynamic check, such as is flying now
        */
        if (moveFlags & MOVEMENTFLAG_FLYING)
        {
            if (moveFlags & MOVEMENTFLAG_BACKWARD /*&& speed_obj.flight >= speed_obj.flight_back*/)
                return MOVE_FLIGHT_BACK;
            else
                return MOVE_FLIGHT;
        }
        else if (moveFlags & MOVEMENTFLAG_SWIMMING)
        {
            if (moveFlags & MOVEMENTFLAG_BACKWARD /*&& speed_obj.swim >= speed_obj.swim_back*/)
                return MOVE_SWIM_BACK;
            else
                return MOVE_SWIM;
        }
        else if (moveFlags & MOVEMENTFLAG_WALKING)
        {
            //if (speed_obj.run > speed_obj.walk)
            return MOVE_WALK;
        }
        else if (moveFlags & MOVEMENTFLAG_BACKWARD /*&& speed_obj.run >= speed_obj.run_back*/)
            return MOVE_RUN_BACK;

        return MOVE_RUN;
    }

    enum MonsterMoveType
    {
        MonsterMoveNormal       = 0,
        MonsterMoveStop         = 1,
        MonsterMoveFacingSpot   = 2,
        MonsterMoveFacingTarget = 3,
        MonsterMoveFacingAngle  = 4
    };

    inline void operator << (ByteBuffer& b, const Vector3& v)
    {
        b << v.x << v.y << v.z;
    }

    inline void operator >> (ByteBuffer& b, Vector3& v)
    {
        b >> v.x >> v.y >> v.z;
    }

    void WriteLinearPath(const Spline<int32>& spline, ByteBuffer& data)
    {
        uint32 last_idx = spline.getPointCount() - 3;
        const Vector3 * real_path = &spline.getPoint(1);

        data << real_path[last_idx];   // destination
        if (last_idx > 0)
        {
            Vector3 middle = (real_path[0] + real_path[last_idx]) / 2.f;
            Vector3 offset;
            // first and last points already appended
            for (uint32 i = 0; i < last_idx; ++i)
            {
                offset = middle - real_path[i];
                data.appendPackXYZ(offset.x, offset.y, offset.z);
            }
        }
    }

    void WriteCatmullRomPath(const Spline<int32>& spline, ByteBuffer& data)
    {
        for (int i = 2; i < spline.getPointCount() - 3; i++)
            data << spline.getPoint(i).y << spline.getPoint(i).x << spline.getPoint(i).z;

        //data.append<Vector3>(&spline.getPoint(2), count);
    }

    void WriteCatmullRomCyclicPath(const Spline<int32>& spline, ByteBuffer& data)
    {
        data << spline.getPoint(1).y << spline.getPoint(1).x << spline.getPoint(1).z; // fake point, client will erase it from the spline after first cycle done
        
        for (int i = 1; i < spline.getPointCount() - 3; i++)
            data << spline.getPoint(i).y << spline.getPoint(i).x << spline.getPoint(i).z;
        //data.append<Vector3>(&spline.getPoint(1), count);
    }

    void MoveSplineInit::Launch()
    {
        MoveSpline& move_spline = *unit.movespline;

        Location real_position(unit.GetPositionX(), unit.GetPositionY(), unit.GetPositionZMinusOffset(), unit.GetOrientation());
        // Elevators also use MOVEMENTFLAG_ONTRANSPORT but we do not keep track of their position changes
        if (unit.GetTransGUID())
        {
            real_position.x = unit.GetTransOffsetX();
            real_position.y = unit.GetTransOffsetY();
            real_position.z = unit.GetTransOffsetZ();
            real_position.orientation = unit.GetTransOffsetO();
        }

        // there is a big chance that current position is unknown if current state is not finalized, need compute it
        // this also allows calculate spline position and update map position in much greater intervals
        // Don't compute for transport movement if the unit is in a motion between two transports
        if (!move_spline.Finalized() && move_spline.onTransport == (unit.GetTransGUID() != 0))
            real_position = move_spline.ComputePosition();

        // should i do the things that user should do? - no.
        if (args.path.empty())
            return;

        // correct first vertex
        args.path[0] = real_position;
        args.initialOrientation = real_position.orientation;
        move_spline.onTransport = (unit.GetTransGUID() != 0);

        uint32 moveFlags = unit.m_movementInfo.GetMovementFlags();
        
        moveFlags |= MOVEMENTFLAG_FORWARD;

		if (moveFlags & MOVEMENTFLAG_ROOT)
			moveFlags &= ~MOVEMENTFLAG_MASK_MOVING;

		if (!args.HasVelocity)
		{
			if (args.flags.walkmode)
				moveFlags |= MOVEMENTFLAG_WALKING;
			else
				moveFlags &= ~MOVEMENTFLAG_WALKING;

            args.velocity = unit.GetSpeed(SelectSpeedType(moveFlags));
		}

        if (!args.Validate())
            return;

        unit.m_movementInfo.SetMovementFlags(moveFlags);
        move_spline.Initialize(args);

        WorldPacket data(SMSG_MONSTER_MOVE, 64);
        ObjectGuid moverGUID = unit.GetGUID();
        ObjectGuid transportGUID = unit.GetTransGUID();
        MoveSplineFlag splineflags =  move_spline.splineflags;
        splineflags.enter_cycle = move_spline.isCyclic();
        uint32 sendSplineFlags = splineflags & ~MoveSplineFlag::Mask_No_Monster_Move;
        int8 seat = unit.GetTransSeat();

        uint8 splineType = 0;

        switch (splineflags & MoveSplineFlag::Mask_Final_Facing)
        {
            case MoveSplineFlag::Final_Target:
                splineType = MonsterMoveFacingTarget;
                break;
            case MoveSplineFlag::Final_Angle:
                splineType = MonsterMoveFacingAngle;
                break;
            case MoveSplineFlag::Final_Point:
                splineType = MonsterMoveFacingSpot;
                break;
            default:
                splineType = MonsterMoveNormal;
                break;
        }

        data << float(move_spline.spline.getPoint(move_spline.spline.first()).z);
        data << float(move_spline.spline.getPoint(move_spline.spline.first()).x);
        data << uint32(move_spline.GetId());
        data << float(move_spline.spline.getPoint(move_spline.spline.first()).y);
        data << float(0.0f); // Most likely transport Y
        data << float(0.0f); // Most likely transport Z
        data << float(0.0f); // Most likely transport X
		
        data.WriteBit(1); // Parabolic speed // esi+4Ch
        data.WriteBit(moverGUID[0]);
        data.WriteBits(splineType, 3);
		
        if (splineType == MonsterMoveFacingTarget)
        {
            ObjectGuid targetGuid = move_spline.facing.target;
            data.WriteBit(targetGuid[6]);
            data.WriteBit(targetGuid[4]);
            data.WriteBit(targetGuid[3]);
            data.WriteBit(targetGuid[0]);
            data.WriteBit(targetGuid[5]);
            data.WriteBit(targetGuid[7]);
            data.WriteBit(targetGuid[1]);
            data.WriteBit(targetGuid[2]);
        }

        data.WriteBit(1);
        data.WriteBit(1);
        data.WriteBit(1);
		
        uint32 uncompressedSplineCount = move_spline.splineflags & MoveSplineFlag::UncompressedPath ? move_spline.splineflags.cyclic ? move_spline.spline.getPointCount() - 2 : move_spline.spline.getPointCount() - 3 : 1;
        data.WriteBits(uncompressedSplineCount,  20);
				
        data.WriteBit(!move_spline.splineflags.raw());
        data.WriteBit(moverGUID[3]);
        data.WriteBit(1);
        data.WriteBit(1);
        data.WriteBit(1);
        data.WriteBit(!move_spline.Duration());
        data.WriteBit(moverGUID[7]);
        data.WriteBit(moverGUID[4]);
        data.WriteBit(1);
        data.WriteBit(moverGUID[5]);

        int32 compressedSplineCount = move_spline.splineflags & MoveSplineFlag::UncompressedPath ? 0 : move_spline.spline.getPointCount() - 3;
        data.WriteBits(compressedSplineCount, 22); // WP count

        data.WriteBit(moverGUID[6]);
        data.WriteBit(0); // Fake bit

        data.WriteBit(transportGUID[7]);
        data.WriteBit(transportGUID[1]);
        data.WriteBit(transportGUID[3]);
        data.WriteBit(transportGUID[0]);
        data.WriteBit(transportGUID[6]);
        data.WriteBit(transportGUID[4]);
        data.WriteBit(transportGUID[5]);
        data.WriteBit(transportGUID[2]);
		
        data.WriteBit(0); // Send no block
        data.WriteBit(0);
        data.WriteBit(moverGUID[2]);
        data.WriteBit(moverGUID[1]);

        data.FlushBits();

        if (compressedSplineCount)
            WriteLinearPath(move_spline.spline, data);

        data.WriteByteSeq(moverGUID[1]);

        data.WriteByteSeq(transportGUID[6]);
        data.WriteByteSeq(transportGUID[4]);
        data.WriteByteSeq(transportGUID[1]);
        data.WriteByteSeq(transportGUID[7]);
        data.WriteByteSeq(transportGUID[0]);
        data.WriteByteSeq(transportGUID[3]);
        data.WriteByteSeq(transportGUID[5]);
        data.WriteByteSeq(transportGUID[2]);
		
        if (splineflags & MoveSplineFlag::UncompressedPath)
        {
            if (splineflags.cyclic)
                WriteCatmullRomCyclicPath(move_spline.spline, data);
            else
                WriteCatmullRomPath(move_spline.spline, data);
        }
        else
        {
            G3D::Vector3 const& point = move_spline.spline.getPoint(move_spline.spline.getPointCount() - 2);
            data << point.y << point.x << point.z;
        }

        if (splineType == MonsterMoveFacingTarget)
        {
            ObjectGuid targetGuid = move_spline.facing.target;
            data.WriteByteSeq(targetGuid[5]);
            data.WriteByteSeq(targetGuid[7]);
            data.WriteByteSeq(targetGuid[0]);
            data.WriteByteSeq(targetGuid[4]);
            data.WriteByteSeq(targetGuid[3]);
            data.WriteByteSeq(targetGuid[2]);
            data.WriteByteSeq(targetGuid[6]);
            data.WriteByteSeq(targetGuid[1]);
        }
		
        data.WriteByteSeq(moverGUID[5]);

        if (splineType == MonsterMoveFacingAngle)
            data << float(move_spline.facing.angle);

        data.WriteByteSeq(moverGUID[3]);

        if (move_spline.splineflags.raw())
            data << uint32(move_spline.splineflags.raw());

        data.WriteByteSeq(moverGUID[6]);

        if (splineType == MonsterMoveFacingSpot)
            data << move_spline.facing.f.x << move_spline.facing.f.y << move_spline.facing.f.z;

        data.WriteByteSeq(moverGUID[0]);
        data.WriteByteSeq(moverGUID[7]);
        data.WriteByteSeq(moverGUID[2]);
        data.WriteByteSeq(moverGUID[4]);

        if (move_spline.Duration())
            data << uint32(move_spline.Duration());
		
        unit.SendMessageToSet(&data, true);
    }

    void MoveSplineInit::Stop(bool force)
    {
        MoveSpline& move_spline = *unit.movespline;

        // No need to stop if we are not moving
        if (move_spline.Finalized())
            return;

        Location loc = move_spline.ComputePosition();
        args.flags = MoveSplineFlag::Done;
        unit.m_movementInfo.RemoveMovementFlag(MOVEMENTFLAG_FORWARD);
        move_spline.Initialize(args);

        WorldPacket data(SMSG_MONSTER_MOVE, 64);
        ObjectGuid moverGUID = unit.GetGUID();
        ObjectGuid transportGUID = unit.GetTransGUID();
		
        data << float(loc.z);
        data << float(loc.x);
        data << uint32(true); //SplineId
        data << float(loc.y);
        data << float(0.f); // Most likely transport Y
        data << float(0.f); // Most likely transport Z
        data << float(0.f); // Most likely transport X
		
        data.WriteBit(1); // Parabolic speed // esi+4Ch
        data.WriteBit(moverGUID[0]);
        data.WriteBits(MonsterMoveStop, 3);
        data.WriteBit(1);
        data.WriteBit(1);
        data.WriteBit(1);
        data.WriteBits(0,  20);
        data.WriteBit(1);
        data.WriteBit(moverGUID[3]);
        data.WriteBit(1);
        data.WriteBit(1);
        data.WriteBit(1);
        data.WriteBit(1);
        data.WriteBit(moverGUID[7]);
        data.WriteBit(moverGUID[4]);
        data.WriteBit(1);
        data.WriteBit(moverGUID[5]);
        data.WriteBits(0, 22); // WP count
        data.WriteBit(moverGUID[6]);
        data.WriteBit(0); // Fake bit
        data.WriteBit(transportGUID[7]);
        data.WriteBit(transportGUID[1]);
        data.WriteBit(transportGUID[3]);
        data.WriteBit(transportGUID[0]);
        data.WriteBit(transportGUID[6]);
        data.WriteBit(transportGUID[4]);
        data.WriteBit(transportGUID[5]);
        data.WriteBit(transportGUID[2]);
        data.WriteBit(0); // Send no block
        data.WriteBit(0);
        data.WriteBit(moverGUID[2]);
        data.WriteBit(moverGUID[1]);

        data.FlushBits();

        data.WriteByteSeq(moverGUID[1]);

        data.WriteByteSeq(transportGUID[6]);
        data.WriteByteSeq(transportGUID[4]);
        data.WriteByteSeq(transportGUID[1]);
        data.WriteByteSeq(transportGUID[7]);
        data.WriteByteSeq(transportGUID[0]);
        data.WriteByteSeq(transportGUID[3]);
        data.WriteByteSeq(transportGUID[5]);
        data.WriteByteSeq(transportGUID[2]);

        data.WriteByteSeq(moverGUID[5]);
        data.WriteByteSeq(moverGUID[3]);
        data.WriteByteSeq(moverGUID[6]);
        data.WriteByteSeq(moverGUID[0]);
        data.WriteByteSeq(moverGUID[7]);
        data.WriteByteSeq(moverGUID[2]);
        data.WriteByteSeq(moverGUID[4]);

        unit.SendMessageToSet(&data, true);
    }

    MoveSplineInit::MoveSplineInit(Unit& m) : unit(m)
    {
        args.splineId = splineIdGen.NewId();
        // Elevators also use MOVEMENTFLAG_ONTRANSPORT but we do not keep track of their position changes
        args.TransformForTransport = unit.GetTransGUID();
        // mix existing state into new
        args.flags.walkmode = unit.m_movementInfo.HasMovementFlag(MOVEMENTFLAG_WALKING);
        args.flags.flying = unit.m_movementInfo.HasMovementFlag(MovementFlags(MOVEMENTFLAG_CAN_FLY | MOVEMENTFLAG_DISABLE_GRAVITY));
        args.flags.smoothGroundPath = true; // enabled by default, CatmullRom mode or client config "pathSmoothing" will disable this
    }

    void MoveSplineInit::SetFacing(const Unit * target)
    {
        args.flags.EnableFacingTarget();
        args.facing.target = target->GetGUID();
    }

    void MoveSplineInit::SetFacing(float angle)
    {
        if (args.TransformForTransport)
        {
            if (Unit* vehicle = unit.GetVehicleBase())
                angle -= vehicle->GetOrientation();
            else if (Transport* transport = unit.GetTransport())
                angle -= transport->GetOrientation();
        }

        args.facing.angle = G3D::wrap(angle, 0.f, (float)G3D::twoPi());
        args.flags.EnableFacingAngle();
    }

    void MoveSplineInit::MoveTo(Vector3 const& dest)
    {
        args.path_Idx_offset = 0;
        args.path.resize(2);
        TransportPathTransform transform(unit, args.TransformForTransport);
        args.path[1] = transform(dest);
    }

    void MoveSplineInit::SetFall()
    {
        args.flags.EnableFalling();
        args.flags.fallingSlow = unit.HasUnitMovementFlag(MOVEMENTFLAG_FALLING_SLOW);
    }

    Vector3 TransportPathTransform::operator()(Vector3 input)
    {
        if (_transformForTransport)
        {
            float unused = 0.0f;
            if (TransportBase* transport = _owner.GetDirectTransport())
                transport->CalculatePassengerOffset(input.x, input.y, input.z, unused);

        }

        return input;
    }
}
