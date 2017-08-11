/***************************************************************************
 *   Copyright (C) 2007 by Lorenzo Miniero (lorenzo.miniero@unina.it)      *
 *   University of Naples Federico II                                      *
 *   COMICS Research Group (http://www.comics.unina.it)                    *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this program; if not, write to the                         *
 *   Free Software Foundation, Inc.,                                       *
 *   59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.             *
 ***************************************************************************/

#ifndef _CONTROL_PACKAGE_H
#define _CONTROL_PACKAGE_H

/*! \file
 *
 * \brief Control Packages Base Headers
 *
 * \author Lorenzo Miniero <lorenzo.miniero@unina.it>
 *
 * \ingroup packages
 * \ref packages
 */

#include <sstream>
#include <map>

// Codecs (unused) and Frames definitions
#include "MediaCtrlCodec.h"

#include "MediaCtrlMemory.h"

using namespace std;
using namespace ost;

namespace mediactrl {

/*! \defgroup packages Control Packages
 */
/*! @{ */

class ControlPackage;
class ControlPackageConnection;
/// List of ControlPackage instances
typedef list<ControlPackage *> ControlPackages;


/// The Control Package Callback abstract class
/**
* @class ControlPackageCallback ControlPackage.h
* An abstract class implemented by the core MediaCtrl object (actually first wrapped by the CfwStack object) in order to receive callback notifications related to requests made by the packages plugins.
* @note This is the only way control packages can actively interact with the core framework: this includes sending REPORT messages related to existing transactions, sending frames to wrapped connections, manipulating media frames and so on.
*/
class ControlPackageCallback {
	public:
		ControlPackageCallback() {}
		virtual ~ControlPackageCallback() {}

		virtual void report(ControlPackage *cp, void *requester, string tid, int status, int timeout, string blob="") = 0;
		virtual void control(ControlPackage *cp, void *requester, string blob) = 0;

		virtual ControlPackageConnection *getConnection(ControlPackage *cp, string conId) = 0;
		virtual ControlPackageConnection *createConference(ControlPackage *cp, string confId) = 0;
		virtual void dropConnection(ControlPackage *cp, ControlPackageConnection *connection) = 0;

		virtual string getPackageConfValue(ControlPackage *cp, string element, string attribute="") = 0;	// FIXME

		virtual void sendFrame(ControlPackageConnection *connection, MediaCtrlFrame *frame) = 0;
		virtual void incomingFrame(ControlPackageConnection *connection, MediaCtrlFrame *frame) = 0;
		virtual void clearDtmfBuffer(ControlPackageConnection *connection) = 0;
		virtual int getNextDtmfBuffer(ControlPackageConnection *connection) = 0;
		virtual uint32_t getFlags(ControlPackageConnection *connection, int mediaType=MEDIACTRL_MEDIA_UNKNOWN) = 0;
		virtual ControlPackageConnection *getSubConnection(ControlPackageConnection *connection, int mediaType) = 0;
		virtual ControlPackageConnection *getSubConnection(ControlPackageConnection *connection, string label) = 0;

		virtual MediaCtrlFrame *decode(MediaCtrlFrame *frame) = 0;
		virtual MediaCtrlFrame *encode(MediaCtrlFrame *frame, int dstFormat) = 0;
};


/// The Control Package base abstract class
/**
* @class ControlPackage ControlPackage.h
* The Control Package abstract class: each new control package plugin must implement this one.
*/
class ControlPackage : public gc, public Thread {
	protected:
		string name;		/*!< Registered name of the package (e.g. "msc-ivr") */
		string version;		/*!< Version of the package (e.g. "1.0") */
		string desc;		/*!< Friendly name of the package */
					/*!< (e.g. "Media Server Control - Interactive Voice Response - version 1.0") */
		string mimeType;	/*!< MIME type of the package (e.g. "application/msc-ivr+xml") */

	public:
		ControlPackage() {}
		virtual ~ControlPackage() {}

		/**
		* @fn setup()
		* Initializes everything related to the package, returning an error if something goes wrong
		* @returns true on success, false otherwise
		* @note This method MUST be implemented and MUST be launched by the core right after the constructor.
		*/
		virtual bool setup() = 0;

		virtual void control(void *requester, string tid, string blob) = 0;	// FIXME
		void setCallback(ControlPackageCallback *callback) { this->callback = callback; };
		virtual void setCollector(void *frameCollector) = 0;

		ControlPackageCallback *callback;	/*!< The ControlPackageCallback instance to talk with in order to actively interact with the core */

		virtual void sendFrame(ControlPackageConnection *connection, MediaCtrlFrame *frame) = 0;
		virtual void incomingFrame(ControlPackageConnection *connection, ControlPackageConnection *subConnection, MediaCtrlFrame *frame) = 0;
		virtual void incomingDtmf(ControlPackageConnection *connection, ControlPackageConnection *subConnection, int type) = 0;
		virtual void frameSent(ControlPackageConnection *connection, ControlPackageConnection *subConnection, MediaCtrlFrame *frame) = 0;
		virtual void connectionClosing(ControlPackageConnection *connection, ControlPackageConnection *subConnection) = 0;
		virtual void connectionLocked(ControlPackageConnection *connection, ControlPackageConnection *subConnection) = 0;
		virtual void connectionUnlocked(ControlPackageConnection *connection, ControlPackageConnection *subConnection) = 0;

		/**
		* @fn getName()
		* Gets the name of the package itself (e.g. "msc-ivr")
		* @returns The name variable
		*/
		string getName() { return name; };
		/**
		* @fn getVersion()
		* Gets the name of the package itself (e.g. "1.0")
		* @returns The version variable
		*/
		string getVersion() { return version; };
		/**
		* @fn getDescription()
		* Gets the description of the package itself (e.g. "Media Server Control - Interactive Voice Response - version 1.0")
		* @returns The desc variable
		*/
		string getDescription() { return desc; };
		/**
		* @fn getMimeType()
		* Gets the MIME type of the package itself (e.g. "application/msc-ivr+xml")
		* @returns The desc variable
		*/
		string getMimeType() { return mimeType; };

		// Add all the verbose info on your package implementing this method
		virtual string getInfo() = 0;
};

/// Class Factories for Control Packages: Control Packages are implemented as plugins, which means that in order to avoid C++ name mangling this class factory has to be used in order to properly create their instances
typedef ControlPackage* create_cp(ControlPackageCallback *callback);
/// Class Factories for Control Packages: Control Packages are implemented as plugins, which means that in order to avoid C++ name mangling this class factory has to be used in order to properly destroy their instances
typedef void destroy_cp(ControlPackage *package);


/// The Control Package Factory class
/**
* @class ControlPackageFactory ControlPackage.h
* The Control Package Factory class: each instance contains the factories to properly create and destroy instances of a plugin.
*/
class ControlPackageFactory : public gc {
	public:
		/**
		* @fn ControlPackageFactory(ControlPackage *pkg, destroy_cp *destroy)
		* Constructor. The ControlPackage instance to handle is specified, as the class factory destructor needed to destroy it.
		* @param pkg The handled ControlPackage instance
		* @param destroy The class factory destructor
		*/
		ControlPackageFactory(ControlPackage *pkg, destroy_cp *destroy)
			{ this->pkg = pkg; this->destroy = destroy; };
		/**
		* @fn ~ControlPackageFactory()
		* Destructor.
		* @note This destructor also makes sure the ControlPackage instance as well is destroyed
		*/
		~ControlPackageFactory() { destroyPackage(); };

		/**
		* @fn getName()
		* Gets the name of the package itself (e.g. "msc-ivr-basic").
		* @returns The package name string
		*/
		string getName() { return pkg->getName(); };

	private:
		/**
		* @fn destroyPackage()
		* Correctly destroys the ControlPackage instance by calling the class factory destructor.
		*/
		void destroyPackage() { destroy(pkg); };

		ControlPackage *pkg;		/*!< The handled ControlPackage instance */
		destroy_cp *destroy;		/*!< A pointer to the class factory destructor */
};

/// Simple token to differentiate connections and conferences in ControlPackageConnection instances
enum {
	/*! Connection */
	CPC_CONNECTION = 0,
	/*! Conference */
	CPC_CONFERENCE,
};

/// The Control Package Connection class
/**
* @class ControlPackageConnection ControlPackage.h
* The Control Package Connection class: this class implements wrappers needed by the plugins to interact with endpoints and send/receive media to/from them.
*/
class ControlPackageConnection : public gc {
	public:
		/**
		* @fn ControlPackageConnection(string Id, string label="")
		* Constructor. Also builds the unique connection identifier
		* @param Id The connection-id (from~to) or conf-id
		* @param label The (optional) label attribute, only valid for connections and not conferences
		* @note If this is a connection and a label is present, the ControlPackageConnection instance wraps a single media channel, otherwise it wraps all the media channels of the SIP session identified by the from~to ID (e.g. from~to --> from~to/label1 AND from~to/label2)
		*/
		ControlPackageConnection(string Id, string label="");
		/**
		* @fn ~ControlPackageConnection()
		* Destructor.
		* @note This does NOT shutdown connections or close media channels... this only removes the wrapper to endpoints, so that control packages can not access them anymore */
		~ControlPackageConnection();

		/**
		* @fn setType(int type)
		* Specifies whether this is a connection or a conference.
		* @param type The connection type
		*/
		void setType(int type) { this->type = type; };
		/**
		* @fn getType()
		* Asks if this is a connection or a conference.
		* @returns The connection type
		*/
		int getType() { return type; };
		/**
		* @fn setMediaType(int mediaType)
		* Specifies whether this is audio or video (-1 = both, in case this is a wrapper)
		* @param mediaType The connection media type
		*/
		void setMediaType(int mediaType) { this->mediaType = mediaType; };
		/**
		* @fn getMediaType()
		* Asks if this is audio or video
		* @returns The connection media type
		*/
		int getMediaType() { return mediaType; };
		/**
		* @fn setPayloadType(int pt)
		* Sets the payload type of the media flowing through this connection (only meaningful for connections and not conferences).
		* @param pt The media payload type
		*/
		void setPayloadType(int pt) { this->pt = pt; };
		/**
		* @fn getPayloadType()
		* Gets the payload type of the media flowing through the connection.
		* @returns The payload type, if specified, -1 otherwise (e.g. for conferences)
		*/
		int getPayloadType() { return pt; };

		/**
		* @fn addPackage(ControlPackage *cp)
		* Adds a control package to the list of packages interested to events related to this connection.
		* @param cp The interested control package
		* @note This method will cause the ControlPackageConnection instance to trigger the package's callbacks for ALL events, including incoming frames, DTMF tones, shutdown events and so on
		*/
		void addPackage(ControlPackage *cp);
		/**
		* @fn removePackage(ControlPackage *cp)
		* Removes a control package to the list of packages interested to events related to this connection.
		* @param cp The interested control package
		* @note This method will cause the ControlPackageConnection instance to stop triggering the package's callbacks for ANY event
		*/
		void removePackage(ControlPackage *cp);

		/**
		* @fn setEndpoint(void *endpoint)
		* Associates the connection with an existing endpoint, which in turn is associated with something else.
		* @param endpoint An opaque pointer to the endpoint
		* @note The pointer is opaque since packages couldn't access endoint methods anyway: the endpoint is only relevant to the classes which will actually operate on it (e.g. CfwStack)
		*/
		void setEndpoint(void *endpoint) { this->endpoint = endpoint; };
		/**
		* @fn getEndpoint()
		* Gets the opaque pointer to the associated endpoint
		* @returns The opaque pointer to the associated endpoint
		* @note As already pointed out, the pointer is opaque, so a casting is needed in order to access its functionality
		*/
		void *getEndpoint() { return endpoint; };
		/**
		* @fn getConnectionId()
		* Gets the unique connection identifier (be it from~to / conf-id)
		* @returns The connection identifier
		*/
		string getConnectionId() { return connectionId; };
		/**
		* @fn getLabel()
		* Gets the label this connection maps to, if any
		* @returns The label, if any, or an empty string otherwise
		*/
		string getLabel() { return label; };

		void sendFrame(MediaCtrlFrame *frame);
		void incomingFrame(ControlPackageConnection *connection, ControlPackageConnection *subConnection, MediaCtrlFrame *frame);
		void frameSent(ControlPackageConnection *connection, ControlPackageConnection *subConnection, MediaCtrlFrame *frame);
		void incomingDtmf(ControlPackageConnection *connection, ControlPackageConnection *subConnection, int type);
		void connectionLocked(ControlPackageConnection *connection, ControlPackageConnection *subConnection);
		void connectionUnlocked(ControlPackageConnection *connection, ControlPackageConnection *subConnection);
		void connectionClosing(ControlPackageConnection *connection, ControlPackageConnection *subConnection);

	protected:
		int type;			/*!< Simple connection or conference? */
		int mediaType;			/*!< audio or video? (-1 = both) */
		void *endpoint;			/*!< Pointer to the actual endpoint */
		string connectionId;		/*!< connection-id (from~to) or conf-id: this value uniquely identifies the connection */
		string Id;			/*!< connection-id (from~to) or conf-id: in case it's a from~to, this connection wraps underlying from~to/label connections */
		string label;			/*!< Label of the media connection, if any */
		int pt;				/*!< The payload type of the media flowing through the connection */

		ControlPackages packages;	/*!< List of packages interested in incoming data from this connection */
		ost::Mutex mPackages;
};


// Common helper classes
/// Common helper classes: Data
/**
* @class XmlData ControlPackage.h
* This class handles instances of the data elements that can be found in XML blobs (see msc-conf-audio and msc-ivr-basic).
* @note This data element will probably be deprecated
*/
class XmlData : public gc {
	public:
		XmlData();
		XmlData(string name, string value) {
			this->name = name;
			this->value = value;
		}
		~XmlData() {};

		string name;	/*!< Name of the data attribute (e.g. "iterations") */
		string value;	/*!< Value of the data attribute (e.g. "4") */
};

/// Common helper classes: Subscribe
/**
* @class Subscription ControlPackage.h
* This class handles instances of the subscribe elements that can be found in XML blobs (see msc-conf-audio and msc-ivr-basic), whenever an AS is interested in events.
* @note This subscription mechanism is still not completely clear in the specification. Besides, events currently carry a sequence of data elements, which as stated before will probably be obsoleted in the near future.
*/
class Subscription : public gc {
	public:
		Subscription();
		Subscription(string name) {
			this->name = name;
			data.clear();
		}
		~Subscription() {};

		void addData(XmlData *data) { this->data.push_back(data); };
		void addData(string name, string value) { this->data.push_back(new XmlData(name, value)); };

		string name;
		list<XmlData *>data;	
};

/// Available media directions
enum media_directions {
	/*! Can send and receive */
	SENDRECV = 0,
	/*! Can only send */
	SENDONLY,
	/*! Can only receive */
	RECVONLY,
	/*! Can neither send nor receive */
	INACTIVE,
};


/// Common helper classes: Stream
/**
* @class MediaStream ControlPackage.h
* This class handles instances of the stream elements that can be found in XML blobs (see msc-conf-audio and msc-ivr-basic), whenever more granularity upon media (e.g. direction) is needed.
* @note This stream mechanism is still not completely clear in the specification, and as of a consequence it still is not as effective as it should be.
*/
class MediaStream : public gc {
	public:
		MediaStream() {
			media = "";
			mediaType = MEDIACTRL_MEDIA_UNKNOWN;
			label = "";
			direction = SENDRECV;
			connection = NULL;
		}
		~MediaStream() {};

		string getDirection() {
			if(direction == INACTIVE)
				return "inactive";
			else if(direction == SENDONLY)
				return "sendonly";
			else if(direction == RECVONLY)
				return "recvonly";
			else if(direction == SENDRECV)
				return "sendrecv";
			return "???";
		};
	
		string media;				/*!< The media type (e.g. "audio") */
		int mediaType;				/*!< The media type (e.g. MEDIACTRL_MEDIA_AUDIO) */
		string label;				/*!< The label associated with this media */
		int direction;				/*!< The media direction (see the media_directions enumerator) */
		ControlPackageConnection *connection;	/*!< The ControlPackageConnection instance associated with this stream */
};

/*! @} */

}

#endif
