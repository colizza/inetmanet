//
// Copyright (C) 2006 Andras Varga
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU Lesser General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU Lesser General Public License for more details.
//
// You should have received a copy of the GNU Lesser General Public License
// along with this program; if not, see <http://www.gnu.org/licenses/>.
//


#include "Ieee80211MgmtAPExtended.h"
#include "Ieee802Ctrl_m.h"
#include "EtherFrame_m.h"
#include "NotifierConsts.h"
#include "RadioState.h"

#include "IP.h"

Define_Module(Ieee80211MgmtAPExtended);

static std::ostream& operator<< (std::ostream& os, const Ieee80211MgmtAPExtended::STAInfo& sta)
{
    os << "state:" << sta.status;
    return os;
}

void Ieee80211MgmtAPExtended::initialize(int stage)
{
    Ieee80211MgmtAPBase::initialize(stage);

    if (stage==0)
    {
        // read params and init vars
        ssid = par("ssid").stringValue();
        beaconInterval = par("beaconInterval");
        numAuthSteps = par("numAuthSteps");
        if (numAuthSteps!=2 && numAuthSteps!=4)
            error("parameter 'numAuthSteps' (number of frames exchanged during authentication) must be 2 or 4, not %d", numAuthSteps);
        channelNumber = -1;  // value will arrive from physical layer in receiveChangeNotification()
        WATCH(ssid);
        WATCH(channelNumber);
        WATCH(beaconInterval);
        WATCH(numAuthSteps);
        WATCH_MAP(staList);

        //TBD fill in supportedRates

        // subscribe for notifications
        NotificationBoard *nb = NotificationBoardAccess().get();
        nb->subscribe(this, NF_RADIO_CHANNEL_CHANGED);

        // start beacon timer (randomize startup time)
        beaconTimer = new cMessage("beaconTimer");
        scheduleAt(simTime()+uniform(0,beaconInterval), beaconTimer);
    }
}

void Ieee80211MgmtAPExtended::handleTimer(cMessage *msg)
{
    if (msg==beaconTimer)
    {
        sendBeacon();
        scheduleAt(simTime()+beaconInterval, beaconTimer);
    }
    else
    {
        error("internal error: unrecognized timer '%s'", msg->getName());
    }
}

void Ieee80211MgmtAPExtended::handleUpperMessage(cPacket *msg)
{
	if (hasRelayUnit) {

		// must be an EtherFrame frame arriving from MACRelayUnit, that is,
		// bridged from another interface of the AP (probably Ethernet).
		EtherFrame *etherframe = check_and_cast<EtherFrame *>(msg);

		// check we really have a STA with that dest address
		STAList::iterator it = staList.find(etherframe->getDest());
		if (it==staList.end() || it->second.status!=ASSOCIATED)
		{
			EV << "STA with MAC address " << etherframe->getDest() << " not associated with this AP, dropping frame\n";
			delete etherframe; // XXX count drops?
			return;
		}

		// convert Ethernet frame
		Ieee80211DataFrame *frame = convertFromEtherFrame(etherframe);
		sendOrEnqueue(frame);
	} else {
		// JcM Add: we assume the packet comes in raw way.

		EV << "Handling upper message from Network Layer" << endl;

		Ieee802Ctrl* ctrl = check_and_cast<Ieee802Ctrl*>(msg->removeControlInfo());

		if (ctrl->getDest()!= MACAddress::BROADCAST_ADDRESS) {
			// check if the destination address is our STA (or broadcast)
			STAList::iterator it = staList.find(ctrl->getDest());
			if (it==staList.end() || it->second.status!=ASSOCIATED)
			{
				EV << "STA with MAC address " << ctrl->getDest() << " not associated with this AP, dropping frame\n";
				delete msg; // XXX count drops?
				return;
			}
		}
		Ieee80211DataFrame *frame = new Ieee80211DataFrame(msg->getName());
		frame->setFromDS(true);

		// copy addresses from ethernet frame (transmitter addr will be set to our addr by MAC)
		frame->setReceiverAddress(ctrl->getDest());
		frame->setAddress3(ctrl->getSrc());

		// encapsulate payload
		frame->encapsulate(msg);
		sendOrEnqueue(frame);
	}
}

#if 0
void Ieee80211MgmtAPExtended::handleUpperMessage(cPacket *msg)
{
    // must be an EtherFrame frame arriving from MACRelayUnit, that is,
    // bridged from another interface of the AP (probably Ethernet).
    EtherFrame *etherframe = check_and_cast<EtherFrame *>(msg);

    // check we really have a STA with that dest address
    STAList::iterator it = staList.find(etherframe->getDest());
    if (it==staList.end() || it->second.status!=ASSOCIATED)
    {
        EV << "STA with MAC address " << etherframe->getDest() << " not associated with this AP, dropping frame\n";
        delete etherframe; // XXX count drops?
        return;
    }

    // convert Ethernet frame
    Ieee80211DataFrame *frame = convertFromEtherFrame(etherframe);
    sendOrEnqueue(frame);
}
#endif

void Ieee80211MgmtAPExtended::handleCommand(int msgkind, cPolymorphic *ctrl)
{
    error("handleCommand(): no commands supported");
}

void Ieee80211MgmtAPExtended::receiveChangeNotification(int category, const cPolymorphic *details)
{
    Enter_Method_Silent();
    printNotificationBanner(category, details);

    if (category == NF_RADIO_CHANNEL_CHANGED)
    {
    	RadioState* rs = check_and_cast<RadioState *>(details);

    	cModule* radio = simulation.getModule(rs->getRadioId());
    	if (radio!=NULL) {
    		if (radio->getParentModule() == this->getParentModule()) {
    			// if the radio that generate the notification is contained in the same module of this mgmt, update the channel
    			this->channelNumber = rs->getChannelNumber();
    			EV << "updating channel number to " << channelNumber << endl;
    		}
    	}
    }
}

Ieee80211MgmtAPExtended::STAInfo *Ieee80211MgmtAPExtended::lookupSenderSTA(Ieee80211ManagementFrame *frame)
{
    STAList::iterator it = staList.find(frame->getTransmitterAddress());
    return it==staList.end() ? NULL : &(it->second);
}

void Ieee80211MgmtAPExtended::sendManagementFrame(Ieee80211ManagementFrame *frame, const MACAddress& destAddr)
{
    frame->setFromDS(true);
    frame->setReceiverAddress(destAddr);
    frame->setAddress3(myAddress);
    sendOrEnqueue(frame);
}

void Ieee80211MgmtAPExtended::sendBeacon()
{
    EV << "Sending beacon\n";
    Ieee80211BeaconFrame *frame = new Ieee80211BeaconFrame("Beacon");
    Ieee80211BeaconFrameBody& body = frame->getBody();
    body.setSSID(ssid.c_str());
    body.setSupportedRates(supportedRates);
    body.setBeaconInterval(beaconInterval);
    body.setChannelNumber(channelNumber);

    frame->setReceiverAddress(MACAddress::BROADCAST_ADDRESS);
    frame->setFromDS(true);

    sendOrEnqueue(frame);
}

void Ieee80211MgmtAPExtended::handleDataFrame(Ieee80211DataFrame *frame)
{
    // check toDS bit
    if (!frame->getToDS())
    {
        // looks like this is not for us - discard
        EV << "Frame is not for us (toDS=false) -- discarding\n";
        delete frame;
        return;
    }

    // handle broadcast frames
    if (frame->getAddress3().isBroadcast())
    {
        EV << "Handling broadcast frame\n";
        if (hasRelayUnit) {
        	// if we have relayUnit, encap the packet in a ethernet frame
            send(convertToEtherFrame((Ieee80211DataFrame *)frame->dup()), "uppergateOut");
        } else {
        	// JcM add: we dont have a relayunit, so, send the decap packet
        	cPacket* payload = frame->decapsulate();
        	delete frame;
			send(payload,"uppergateOut");
        }
        // JcM Fix: avoid the redistribution of the message to the radio
        //distributeReceivedDataFrame(frame);
        return;
    }

    // look up destination address in our STA list
    STAList::iterator it = staList.find(frame->getAddress3());
    if (it==staList.end())
    {
        // not our STA -- pass up frame to relayUnit for LAN bridging if we have one
        if (hasRelayUnit) {
            send(convertToEtherFrame(frame), "uppergateOut");
        } else {
			// JcM add: we dont have a relayunit, so, send the decap packet
        	cPacket* payload = frame->decapsulate();
			delete frame;
			send(payload,"uppergateOut");
        }
    }
    else
    {
        // dest address is our STA, but is it already associated?
        if (it->second.status == ASSOCIATED)
            distributeReceivedDataFrame(frame); // send it out to the destination STA
        else {
            EV << "Frame's destination STA is not in associated state -- dropping frame\n";
            delete frame;
        }
    }
}
#if 0

void Ieee80211MgmtAPExtended::handleDataFrame(Ieee80211DataFrame *frame)
{
    // check toDS bit
    if (!frame->getToDS())
    {
        // looks like this is not for us - discard
        EV << "Frame is not for us (toDS=false) -- discarding\n";
        delete frame;
        return;
    }

    // handle broadcast frames
    if (frame->getAddress3().isBroadcast())
    {
        EV << "Handling broadcast frame\n";
        if (hasRelayUnit)
            send(convertToEtherFrame((Ieee80211DataFrame *)frame->dup()), "uppergateOut");
        distributeReceivedDataFrame(frame);
        return;
    }

    // look up destination address in our STA list
    STAList::iterator it = staList.find(frame->getAddress3());
    if (it==staList.end())
    {
        // not our STA -- pass up frame to relayUnit for LAN bridging if we have one
        if (hasRelayUnit)
            send(convertToEtherFrame(frame), "uppergateOut");
        else {
            EV << "Frame's destination address is not in our STA list -- dropping frame\n";
            delete frame;
        }
    }
    else
    {
        // dest address is our STA, but is it already associated?
        if (it->second.status == ASSOCIATED)
            distributeReceivedDataFrame(frame); // send it out to the destination STA
        else {
            EV << "Frame's destination STA is not in associated state -- dropping frame\n";
            delete frame;
        }
    }
}

#endif

void Ieee80211MgmtAPExtended::handleAuthenticationFrame(Ieee80211AuthenticationFrame *frame)
{
    int frameAuthSeq = frame->getBody().getSequenceNumber();
    EV << "Processing Authentication frame, seqNum=" << frameAuthSeq << "\n";

    // create STA entry if needed
    STAInfo *sta = lookupSenderSTA(frame);
    if (!sta)
    {
        MACAddress staAddress = frame->getTransmitterAddress();
        sta = &staList[staAddress]; // this implicitly creates a new entry
        sta->address = staAddress;
        sta->status = NOT_AUTHENTICATED;
        sta->authSeqExpected = 1;
    }

    // check authentication sequence number is OK
    if (frameAuthSeq != sta->authSeqExpected)
    {
        // wrong sequence number: send error and return
        EV << "Wrong sequence number, " << sta->authSeqExpected << " expected\n";
        Ieee80211AuthenticationFrame *resp = new Ieee80211AuthenticationFrame("Auth-ERROR");
        resp->getBody().setStatusCode(SC_AUTH_OUT_OF_SEQ);
        sendManagementFrame(resp, frame->getTransmitterAddress());
        delete frame;
        sta->authSeqExpected = 1; // go back to start square
        return;
    }

    // station is authenticated if it made it through the required number of steps
    bool isLast = (frameAuthSeq+1 == numAuthSteps);

    // send OK response (we don't model the cryptography part, just assume
    // successful authentication every time)
    EV << "Sending Authentication frame, seqNum=" << (frameAuthSeq+1) << "\n";
    Ieee80211AuthenticationFrame *resp = new Ieee80211AuthenticationFrame(isLast ? "Auth-OK" : "Auth");
    resp->getBody().setSequenceNumber(frameAuthSeq+1);
    resp->getBody().setStatusCode(SC_SUCCESSFUL);
    resp->getBody().setIsLast(isLast);
    // XXX frame length could be increased to account for challenge text length etc.
    sendManagementFrame(resp, frame->getTransmitterAddress());

    delete frame;

    // update status
    if (isLast)
    {
        sta->status = AUTHENTICATED; // XXX only when ACK of this frame arrives
        EV << "STA authenticated\n";
    }
    else
    {
        sta->authSeqExpected += 2;
        EV << "Expecting Authentication frame " << sta->authSeqExpected << "\n";
    }
}

void Ieee80211MgmtAPExtended::handleDeauthenticationFrame(Ieee80211DeauthenticationFrame *frame)
{
    EV << "Processing Deauthentication frame\n";

    STAInfo *sta = lookupSenderSTA(frame);
    delete frame;

    if (sta)
    {
        // mark STA as not authenticated; alternatively, it could also be removed from staList
        sta->status = NOT_AUTHENTICATED;
        sta->authSeqExpected = 1;
    }
}

void Ieee80211MgmtAPExtended::handleAssociationRequestFrame(Ieee80211AssociationRequestFrame *frame)
{
    EV << "Processing AssociationRequest frame\n";

    // "11.3.2 AP association procedures"
    STAInfo *sta = lookupSenderSTA(frame);
    if (!sta || sta->status==NOT_AUTHENTICATED)
    {
        // STA not authenticated: send error and return
        Ieee80211DeauthenticationFrame *resp = new Ieee80211DeauthenticationFrame("Deauth");
        resp->getBody().setReasonCode(RC_NONAUTH_ASS_REQUEST);
        sendManagementFrame(resp, frame->getTransmitterAddress());
        delete frame;
        return;
    }

    delete frame;

    // mark STA as associated
    sta->status = ASSOCIATED; // XXX this should only take place when MAC receives the ACK for the response

    // send OK response
    Ieee80211AssociationResponseFrame *resp = new Ieee80211AssociationResponseFrame("AssocResp-OK");
    Ieee80211AssociationResponseFrameBody& body = resp->getBody();
    body.setStatusCode(SC_SUCCESSFUL);
    body.setAid(0); //XXX
    body.setSupportedRates(supportedRates);
    sendManagementFrame(resp, sta->address);
}

void Ieee80211MgmtAPExtended::handleAssociationResponseFrame(Ieee80211AssociationResponseFrame *frame)
{
    dropManagementFrame(frame);
}

void Ieee80211MgmtAPExtended::handleReassociationRequestFrame(Ieee80211ReassociationRequestFrame *frame)
{
    EV << "Processing ReassociationRequest frame\n";

    // "11.3.4 AP reassociation procedures" -- almost the same as AssociationRequest processing
    STAInfo *sta = lookupSenderSTA(frame);
    if (!sta || sta->status==NOT_AUTHENTICATED)
    {
        // STA not authenticated: send error and return
        Ieee80211DeauthenticationFrame *resp = new Ieee80211DeauthenticationFrame("Deauth");
        resp->getBody().setReasonCode(RC_NONAUTH_ASS_REQUEST);
        sendManagementFrame(resp, frame->getTransmitterAddress());
        delete frame;
        return;
    }

    delete frame;

    // mark STA as associated
    sta->status = ASSOCIATED; // XXX this should only take place when MAC receives the ACK for the response

    // send OK response
    Ieee80211ReassociationResponseFrame *resp = new Ieee80211ReassociationResponseFrame("ReassocResp-OK");
    Ieee80211ReassociationResponseFrameBody& body = resp->getBody();
    body.setStatusCode(SC_SUCCESSFUL);
    body.setAid(0); //XXX
    body.setSupportedRates(supportedRates);
    sendManagementFrame(resp, sta->address);
}

void Ieee80211MgmtAPExtended::handleReassociationResponseFrame(Ieee80211ReassociationResponseFrame *frame)
{
    dropManagementFrame(frame);
}

void Ieee80211MgmtAPExtended::handleDisassociationFrame(Ieee80211DisassociationFrame *frame)
{
    STAInfo *sta = lookupSenderSTA(frame);
    delete frame;

    if (sta)
    {
        sta->status = AUTHENTICATED;
    }
}

void Ieee80211MgmtAPExtended::handleBeaconFrame(Ieee80211BeaconFrame *frame)
{
    dropManagementFrame(frame);
}

void Ieee80211MgmtAPExtended::handleProbeRequestFrame(Ieee80211ProbeRequestFrame *frame)
{
    EV << "Processing ProbeRequest frame\n";

    if (strcmp(frame->getBody().getSSID(),"")!=0 && strcmp(frame->getBody().getSSID(), ssid.c_str())!=0)
    {
        EV << "SSID `" << frame->getBody().getSSID() << "' does not match, ignoring frame\n";
        dropManagementFrame(frame);
        return;
    }

    MACAddress staAddress = frame->getTransmitterAddress();
    delete frame;

    EV << "Sending ProbeResponse frame\n";
    Ieee80211ProbeResponseFrame *resp = new Ieee80211ProbeResponseFrame("ProbeResp");
    Ieee80211ProbeResponseFrameBody& body = resp->getBody();
    body.setSSID(ssid.c_str());
    body.setSupportedRates(supportedRates);
    body.setBeaconInterval(beaconInterval);
    body.setChannelNumber(channelNumber);
    sendManagementFrame(resp, staAddress);
}

void Ieee80211MgmtAPExtended::handleProbeResponseFrame(Ieee80211ProbeResponseFrame *frame)
{
    dropManagementFrame(frame);
}


