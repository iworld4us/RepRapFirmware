/*
 * Kinematics.cpp
 *
 *  Created on: 24 Apr 2017
 *      Author: David
 */

#include "Kinematics.h"

#include "CoreKinematics.h"
#include "LinearDeltaKinematics.h"
#include "RotaryDeltaKinematics.h"
#include "ScaraKinematics.h"
#include "HangprinterKinematics.h"
#include "PolarKinematics.h"
//#include "FiveBarScaraKinematics.h"

#include <Platform/RepRap.h>
#include <Platform/Platform.h>
#include <GCodes/GCodes.h>
#include <GCodes/GCodeBuffer/GCodeBuffer.h>

const char * const Kinematics::HomeAllFileName = "homeall.g";

// Constructor. Pass segsPerSecond <= 0.0 to get non-segmented kinematics.
Kinematics::Kinematics(KinematicsType t, SegmentationType segType) noexcept
	: segmentsPerSecond(DefaultSegmentsPerSecond), minSegmentLength(DefaultMinSegmentLength), reciprocalMinSegmentLength(1.0/DefaultMinSegmentLength),
	  segmentationType(segType), type(t)
{
}

// Set or report the parameters from a M665, M666 or M669 command
// This is the fallback function for when the derived class doesn't use the specified M-code
bool Kinematics::Configure(unsigned int mCode, GCodeBuffer& gb, const StringRef& reply, bool& error) THROWS(GCodeException)
{
	if (mCode == 669)
	{
		if (!gb.Seen('K'))
		{
			reply.printf("Kinematics is %s", GetName());
		}
	}
	else
	{
		reply.printf("M%u parameters do not apply to %s kinematics", mCode, GetName());
		error = true;
	}
	return false;
}

// Try to configure the segmentation parameters. Return true if we did.
bool Kinematics::TryConfigureSegmentation(GCodeBuffer& gb) noexcept
{
	bool seen = false;
	gb.TryGetFValue('S', segmentsPerSecond, seen);
	gb.TryGetFValue('T', minSegmentLength, seen);
	if (seen)
	{
		segmentationType.useSegmentation = minSegmentLength > 0.0 && segmentsPerSecond > 0.0;
		if (segmentationType.useSegmentation)
		{
			reciprocalMinSegmentLength = 1.0 / minSegmentLength;
		}
	}
	return seen;
}

// Return true if the specified XY position is reachable by the print head reference point.
// This default implementation assumes a rectangular reachable area, so it just uses the bed dimensions give in the M208 command.
bool Kinematics::IsReachable(float axesCoords[MaxAxes], AxesBitmap axes, bool isCoordinated) const noexcept
{
	const Platform& platform = reprap.GetPlatform();
	return axes.IterateWhile([&platform, axesCoords](unsigned int axis, unsigned int count) -> bool {
		if (axesCoords[axis] >= platform.AxisMinimum(axis) && axesCoords[axis] <= platform.AxisMaximum(axis))
		{
			return true;
		}
		return false;
	});
}

// Limit the Cartesian position that the user wants to move to, returning true if any coordinates were changed
// This default implementation just applies the rectangular limits set up by M208 to those axes that have been homed.
LimitPositionResult Kinematics::LimitPosition(float finalCoords[], const float * null initialCoords,
												size_t numVisibleAxes, AxesBitmap axesToLimit, bool isCoordinated, bool applyM208Limits) const noexcept
{
	return (applyM208Limits && LimitPositionFromAxis(finalCoords, 0, numVisibleAxes, axesToLimit)) ? LimitPositionResult::adjusted : LimitPositionResult::ok;
}

// Apply the M208 limits to the Cartesian position that the user wants to move to for all axes from the specified one upwards
// Return true if any coordinates were changed
bool Kinematics::LimitPositionFromAxis(float coords[], size_t firstAxis, size_t numVisibleAxes, AxesBitmap axesToLimit) const noexcept
{
	const Platform& platform = reprap.GetPlatform();
	bool limited = false;
	for (size_t axis = firstAxis; axis < numVisibleAxes; axis++)
	{
		if (axesToLimit.IsBitSet(axis))
		{
			float& f = coords[axis];
			// When homing a printer we convert the M208 axis limit to motor positions, then back again to get the user position.
			// This value may not round-trip exactly due to rounding error and quantisation to the nearest step, especially if the steps/mm is not integral.
			// So we allow a small error here without considering it to be out of limits.
			if (f < platform.AxisMinimum(axis) - AxisRoundingError)
			{
				f = platform.AxisMinimum(axis);
				limited = true;
			}
			else if (f > platform.AxisMaximum(axis) + AxisRoundingError)
			{
				f = platform.AxisMaximum(axis);
				limited = true;
			}
		}
	}
	return limited;
}

// Return the initial Cartesian coordinates we assume after switching to this kinematics
// This default is suitable for Cartesian and CoreXY printers.
void Kinematics::GetAssumedInitialPosition(size_t numAxes, float positions[]) const noexcept
{
	for (size_t i = 0; i < numAxes; ++i)
	{
		positions[i] = 0.0;
	}
}

// This function is called when a request is made to home the axes in 'toBeHomed' and the axes in 'alreadyHomed' have already been homed.
// If we can proceed with homing some axes, return the name of the homing file to be called.
// If we can't proceed because other axes need to be homed first, return nullptr and pass those axes back in 'mustBeHomedFirst'.
// This default is suitable for most kinematics.
AxesBitmap Kinematics::GetHomingFileName(AxesBitmap toBeHomed, AxesBitmap alreadyHomed, size_t numVisibleAxes, const StringRef& filename) const noexcept
{
	const AxesBitmap allAxes = AxesBitmap::MakeLowestNBits(numVisibleAxes);
	if ((toBeHomed & allAxes) == allAxes)
	{
		filename.copy(HomeAllFileName);
		return AxesBitmap();
	}

	// If Z homing is done using a Z probe then X and Y must be homed before Z
	const bool homeZLast = (toBeHomed.IsBitSet(Z_AXIS) && reprap.GetPlatform().GetEndstops().HomingZWithProbe());
	const AxesBitmap homeFirst = AxesToHomeBeforeProbing();

	// Return the homing file for the lowest axis that we have been asked to home
	for (size_t axis = 0; axis < numVisibleAxes; ++axis)
	{
		if (toBeHomed.IsBitSet(axis) && (axis != Z_AXIS || !homeZLast || (alreadyHomed & homeFirst) == homeFirst))
		{
			filename.copy("home");
			filename.cat(tolower(reprap.GetGCodes().GetAxisLetters()[axis]));
			filename.cat(".g");
			return AxesBitmap();
		}
	}

	// Error, we can't home any axes that we were asked to home. It can only be because we can't home the Z axis.
	return homeFirst & ~alreadyHomed;
}

// Return a bitmap of the motors that affect this axis or tower. Used for implementing stall detection endstops and energising additional motors.
// Usually it is just the corresponding motor (hence this default implementation), but CoreXY and similar kinematics move multiple motors to home an individual axis.
AxesBitmap Kinematics::GetConnectedAxes(size_t axis) const noexcept
{
	return AxesBitmap::MakeFromBits(axis);
}

// Return true if the specified axis is a continuous rotation axis. This default implementation is overridden in some classes e.g. polar.
bool Kinematics::IsContinuousRotationAxis(size_t axis) const noexcept
{
	return false;
}

/*static*/ Kinematics *Kinematics::Create(KinematicsType k) noexcept
{
	switch (k)
	{
	default:
		return nullptr;

	case KinematicsType::cartesian:
	case KinematicsType::coreXY:
	case KinematicsType::coreXZ:
	case KinematicsType::coreXYU:
	case KinematicsType::coreXYUV:
	case KinematicsType::markForged:
		return new CoreKinematics(k);
//		return new CoreKinematics(KinematicsType::cartesian);

	case KinematicsType::linearDelta:
		return new LinearDeltaKinematics();
	case KinematicsType::scara:
		return new ScaraKinematics();
	case KinematicsType::hangprinter:
		return new HangprinterKinematics();
	case KinematicsType::polar:
		return new PolarKinematics();
	case KinematicsType::rotaryDelta:
		return new RotaryDeltaKinematics();
	//case KinematicsType::fiveBarScara:
	//	return new FiveBarScaraKinematics();
	}
}

/*static*/ void Kinematics::PrintMatrix(const char* s, const MathMatrix<float>& m, size_t maxRows, size_t maxCols) noexcept
{
	debugPrintf("%s\n", s);
	if (maxRows == 0)
	{
		maxRows = m.rows();
	}
	if (maxCols == 0)
	{
		maxCols = m.cols();
	}

	for (size_t i = 0; i < maxRows; ++i)
	{
		for (size_t j = 0; j < maxCols; ++j)
		{
			debugPrintf("%7.4f%c", (double)m(i, j), (j == maxCols - 1) ? '\n' : ' ');
		}
	}
}

/*static*/ void Kinematics::PrintMatrix(const char* s, const MathMatrix<double>& m, size_t maxRows, size_t maxCols) noexcept
{
	debugPrintf("%s\n", s);
	if (maxRows == 0)
	{
		maxRows = m.rows();
	}
	if (maxCols == 0)
	{
		maxCols = m.cols();
	}

	for (size_t i = 0; i < maxRows; ++i)
	{
		for (size_t j = 0; j < maxCols; ++j)
		{
			debugPrintf("%7.4f%c", m(i, j), (j == maxCols - 1) ? '\n' : ' ');
		}
	}
}

/*static*/ void Kinematics::PrintVector(const char *s, const float *v, size_t numElems) noexcept
{
	debugPrintf("%s:", s);
	for (size_t i = 0; i < numElems; ++i)
	{
		debugPrintf(" %7.4f", (double)v[i]);
	}
	debugPrintf("\n");
}

/*static*/ void Kinematics::PrintVector(const char *s, const double *v, size_t numElems) noexcept
{
	debugPrintf("%s:", s);
	for (size_t i = 0; i < numElems; ++i)
	{
		debugPrintf(" %7.4f", v[i]);
	}
	debugPrintf("\n");
}

// End
