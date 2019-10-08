/*
 * RemoteFan.cpp
 *
 *  Created on: 3 Sep 2019
 *      Author: David
 */

#include "RemoteFan.h"

#if SUPPORT_CAN_EXPANSION

#include "CanMessageBuffer.h"
#include "CAN/CanInterface.h"
#include "CAN/CanMessageGenericConstructor.h"

RemoteFan::RemoteFan(unsigned int fanNum, CanAddress boardNum)
	: Fan(fanNum),
	  lastRpm(-1), whenLastRpmReceived(0), boardNumber(boardNum), thermostaticFanRunning(false)
{
}

RemoteFan::~RemoteFan()
{
	CanMessageGenericConstructor cons(M950FanParams);
	cons.AddUParam('F', fanNumber);
	cons.AddStringParam('C', "nil");
	String<1> dummy;
	(void)cons.SendAndGetResponse(CanMessageType::m950Fan, boardNumber, dummy.GetRef());
}

bool RemoteFan::Check()
{
	return thermostaticFanRunning;
}

bool RemoteFan::IsEnabled() const
{
	return true;
}

GCodeResult RemoteFan::SetPwmFrequency(PwmFrequency freq, const StringRef& reply)
{
	CanMessageGenericConstructor cons(M950FanParams);
	cons.AddUParam('F', fanNumber);
	cons.AddUParam('Q', freq);
	return cons.SendAndGetResponse(CanMessageType::m950Fan, boardNumber, reply);
}

void RemoteFan::UpdateRpmFromRemote(CanAddress src, int32_t rpm)
{
	if (src == boardNumber)
	{
		lastRpm = rpm;
		whenLastRpmReceived = millis();
	}
}

int32_t RemoteFan::GetRPM()
{
	if (millis() - whenLastRpmReceived > RpmReadingTimeout)
	{
		lastRpm = -1;
	}
	return lastRpm;
}

GCodeResult RemoteFan::ReportPortDetails(const StringRef& str) const
{
	CanMessageGenericConstructor cons(M950FanParams);
	cons.AddUParam('F', fanNumber);
	return cons.SendAndGetResponse(CanMessageType::m950Fan, boardNumber, str);
}

bool RemoteFan::UpdateFanConfiguration(const StringRef& reply)
{
	CanMessageBuffer *buf = CanMessageBuffer::Allocate();
	if (buf == nullptr)
	{
		reply.copy("No CAN buffer available");
		return false;
	}

	const CanRequestId rid = CanInterface::AllocateRequestId(boardNumber);
	auto msg = buf->SetupRequestMessage<CanMessageFanParameters>(rid, CanId::MasterAddress, boardNumber);
	msg->fanNumber = fanNumber;
	msg->blipTime = blipTime;
	msg->val = val;
	msg->minVal = minVal;
	msg->maxVal = maxVal;
	msg->triggerTemperatures[0] = triggerTemperatures[0];
	msg->triggerTemperatures[1] = triggerTemperatures[1];
	msg->sensorsMonitored = sensorsMonitored;

	return CanInterface::SendRequestAndGetStandardReply(buf, rid, reply) == GCodeResult::ok;
}

// Update the hardware PWM
GCodeResult RemoteFan::Refresh(const StringRef& reply)
{
	CanMessageBuffer *buf = CanMessageBuffer::Allocate();
	if (buf == nullptr)
	{
		reply.copy("No CAN buffer available");
		return GCodeResult::error;
	}

	const CanRequestId rid = CanInterface::AllocateRequestId(boardNumber);
	auto msg = buf->SetupRequestMessage<CanMessageSetFanSpeed>(rid, CanId::MasterAddress, boardNumber);
	msg->fanNumber = fanNumber;
	msg->pwm = val;
	return CanInterface::SendRequestAndGetStandardReply(buf, rid, reply);
}

GCodeResult RemoteFan::ConfigurePort(const char* pinNames, PwmFrequency freq, const StringRef& reply)
{
	CanMessageGenericConstructor cons(M950FanParams);
	cons.AddUParam('F', fanNumber);
	cons.AddUParam('Q', freq);
	cons.AddStringParam('C', pinNames);
	return cons.SendAndGetResponse(CanMessageType::m950Fan, boardNumber, reply);
}

#endif

// End
