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

#ifndef _MEDIA_CTRL_CODEC_H
#define _MEDIA_CTRL_CODEC_H

/*! \file
 *
 * \brief Media Codecs Base Headers
 *
 * \author Lorenzo Miniero <lorenzo.miniero@unina.it>
 *
 * \ingroup codecs
 * \ref codecs
 */


#include <iostream>
#include <sstream>
#include <cc++/thread.h>
#include <boost/regex.hpp>

#include "MediaCtrlMemory.h"

using namespace std;
using namespace boost;


void startCollector(void);
void stopCollector(void);
void *getCollector();
void setCommonCollector(void *frameCollector);


namespace mediactrl {

/*! \defgroup codecs Media Codecs
 */
/*! @{ */

/// Identifiers to address the frame content
enum {
	/*! Unknown frame type */
	MEDIACTRL_MEDIA_UNKNOWN = -1,
	/*! Audio frame (default) */
	MEDIACTRL_MEDIA_AUDIO,
};

/// Identifiers to handle codecs
enum {
	/*! Raw frame (not encoded) */
	MEDIACTRL_RAW = -1,
	/*! mU-law frame */
	MEDIACTRL_CODEC_ULAW = 0,
	/*! GSM frame */
	MEDIACTRL_CODEC_GSM = 3,
	/*! A-law frame */
	MEDIACTRL_CODEC_ALAW = 8,
	/*! H.261 frame */
	MEDIACTRL_CODEC_H261 = 31,
	/*! H.263 frame */
	MEDIACTRL_CODEC_H263 = 34,
	/*! H.263+ frame */
	MEDIACTRL_CODEC_H263PLUS = 200,		// FIXME How to handle dynamic AVTs?
	/*! H.264 frame */
	MEDIACTRL_CODEC_H264 = 99,			// FIXME Both Asterisk and the Grandstream GXV3000 use this
};

/// Optional flags to specify frame-related information
enum {
	/*! No flag (the default) */
	MEDIACTRL_FLAG_NONE = 	0,
	/*! QCIF */
	MEDIACTRL_FLAG_QCIF = 	(1 << 0),
	/*! CIF */
	MEDIACTRL_FLAG_CIF = 	(1 << 1),
};


typedef list<int> DtmfTones;
/// Identifiers for DTMF tones, excluding A, B, C and D
enum {
	/*! no tone */
	MEDIACTRL_DTMF_NONE = -1,
	/*! 0 */
	MEDIACTRL_DTMF_0 = 0,
	/*! 1 */
	MEDIACTRL_DTMF_1,
	/*! 2 */
	MEDIACTRL_DTMF_2,
	/*! 3 */
	MEDIACTRL_DTMF_3,
	/*! 4 */
	MEDIACTRL_DTMF_4,
	/*! 5 */
	MEDIACTRL_DTMF_5,
	/*! 6 */
	MEDIACTRL_DTMF_6,
	/*! 7 */
	MEDIACTRL_DTMF_7,
	/*! 8 */
	MEDIACTRL_DTMF_8,
	/*! 9 */
	MEDIACTRL_DTMF_9,
	/*! * */
	MEDIACTRL_DTMF_STAR,
	/*! # */
	MEDIACTRL_DTMF_POUND,
	/*! A */
	MEDIACTRL_DTMF_A,
	/*! B */
	MEDIACTRL_DTMF_B,
	/*! C */
	MEDIACTRL_DTMF_C,
	/*! D */
	MEDIACTRL_DTMF_D,
};


/// Debug only
#define RTP		1
#define IVR		2
#define MIXER	3
#define CODEC	4


class MediaCtrlCodec;	// The Codec Class
class MediaCtrlFrame;	// The media frame format (format+buffer+len)
typedef list<MediaCtrlFrame *> MediaCtrlFrames;

/// Class Factories for Codecs: Codecs are implemented as plugins, which means that in order to avoid C++ name mangling this class factory has to be used in order to properly create their instances
typedef MediaCtrlCodec* create_cd();
/// Class Factories for Codecs: Codecs are implemented as plugins, which means that in order to avoid C++ name mangling this class factory has to be used in order to properly destroy their instances
typedef void destroy_cd(MediaCtrlCodec *);
/// This method is invoked to free everything the codec has setup globally: the core calls it before closing the shared object
typedef void purge_cd(void);


/// Codec Factory
/**
* @class CodecFactory MediaCtrlCodec.h
* The Codec Factory class: each instance contains the factories to properly create and destroy instances of a plugin.
*/
class CodecFactory : public gc {
	public:
		/**
		* @fn CodecFactory(string name, string nameMask, create_cd *create, destroy_cd *destroy, purge_cd *purge, int blockLen)
		* Constructor. Creates a new codec factory for a specific codec (plugin),
		*/
		CodecFactory(string name, string nameMask, create_cd *create, destroy_cd *destroy, purge_cd *purge, int blockLen)
			{
				this->name = name;
				this->nameMask = nameMask;
				this->create = create;
				this->destroy = destroy;
				this->purge = purge;
				this->blockLen = blockLen;
			};
		/**
		* @fn ~CodecFactory()
		* Destructor.
		*/
		~CodecFactory() {};

		create_cd *create;	/*!< The constructor codec factory */
		destroy_cd *destroy;	/*!< The destructor codec factory */
		purge_cd *purge;	/*!< The purge codec factory */

		/**
		* @fn getName()
		* Gets the name of the codec (e.g. "GSM")
		* @returns The name of the codec
		*/
		string getName() { return name; };
		/**
		* @fn getBlockLen()
		* Gets the length of a generic frame for this codec (e.g. 33 for GSM)
		* @returns The block length
		*/
		int getBlockLen() { return blockLen; };
		/**
		* @fn checkName(string name)
		* Method to check if the provided AVT name is valid for this codec (to check dynamic AVTs by name).
		* @param name A string containing the AVT name
		* @returns true if it is, false otherwise
		*/
		bool checkName(string name)
			{
				regex re;
				re.assign(nameMask, regex_constants::icase);
				if(!regex_match(name.c_str(), re))
					return false;
				return true;
			};

	private:
		string name;		/*!< Name of the codec (e.g. "GSM") */
		string nameMask;	/*!< Allowed names as a Boost::Regex mask (e.g. "H263|H.263") */
		int blockLen;		/*!< Typical frame length for this codec (e.g 33 for GSM) */
};


/// Codec base abstract class
/**
* @class MediaCtrlCodec MediaCtrlCodec.h
* The Codec abtract class.
*/
class MediaCtrlCodec : public gc {
	protected:
		int media;		/*!< The media type of codec (e.g. MEDIACTRL_MEDIA_AUDIO for GSM) */
		int format;		/*!< Format (e.g. 3 for GSM) */
		uint32_t clockrate;	/*!< Clock Rate (e.g. 8000 for GSM) */
		list<uint16_t >avts;	/*!< List of AVT profiles associated with this codec (currently only one, same as format) */

		string name;		/*!< Name of the codec (e.g. "GSM") */
		string nameMask;	/*!< Allowed names as a Boost::Regex mask (e.g. "H263|H.263") */

		uint16_t blockLen;	/*!< Typical frame length for this codec (e.g 33 for GSM) */

		bool started;		/*!< True when the codec has been successfully started */

	public:
		/**
		* @fn MediaCtrlCodec()
		* Constructor. Just fills in the default values.
		*/
		MediaCtrlCodec() {}
		virtual ~MediaCtrlCodec() {}

		virtual void setCollector(void *frameCollector) = 0;

		/**
		* @fn start()
		* Virtual method that actually initializes the codec.
		*/
		virtual bool start() = 0;
		/**
		* @fn hasStarted()
		* Checks if the codec has already been started or not
		* @returns true if it has, false otherwise
		*/
		bool hasStarted() { return started; };

		/**
		* @fn getName()
		* Returns the name of the codec
		* @returns The name of the codec
		*/
		string getName() { return name; };
		/**
		* @fn getNameMask()
		* Returns the name mask of the codec
		* @returns The name mask of the codec
		*/
		string getNameMask() { return nameMask; };
		/**
		* @fn getMediaType()
		* Returns the codec handled media type
		* @returns The codec media type (MEDIACTRL_MEDIA_AUDIO)
		*/
		int getMediaType() { return media; };
		/**
		* @fn getCodecId()
		* Returns the codec identifier
		* @returns The codec identifier
		*/
		int getCodecId() { return format; };
		/**
		* @fn getClockRate()
		* Returns the codec clock rate
		* @returns The codec clock rate
		*/
		uint32_t getClockRate() { return clockrate; };
		/**
		* @fn getBlockLen()
		* Returns the block length of a frame of the codec
		* @returns The block length
		*/
		uint16_t getBlockLen() { return blockLen; };

		/**
		* @fn checkAvt(int avt)
		* Virtual method to check if the provided AVT profile identifier is valid for this codec.
		* @param avt A numeric AVT profile identifier
		* @returns true if it is, false otherwise
		*/
		virtual bool checkAvt(int avt) = 0;

		/**
		* @fn addSetting(string var, string value)
		* Virtual method to pass to the codec some specific settings before startup
		* @param var The name of the setting
		* @param value The value(s) for the setting
		*/
		virtual void addSetting(string var, string value) = 0;
		/**
		* @fn getSetting(string var)
		* Method to query the codec about some specific setting
		* @param var The name of the setting
		* @returns A string with value(s) for the setting
		* @note By default the method returns an empty string, since usually codecs won't need it; this means that new codecs are not mandated to implement it.
		*/
		string getSetting(string var) { return ""; };

		/**
		* @fn encode(MediaCtrlFrame *outgoing)
		* Virtual method to encode a raw frame with the codec
		* @param outgoing The raw frame to encode
		* @returns The encoded frame if successful, NULL otherwise
		* @note The method expects a raw frame, because each plugin has no means to previously decode a frame encoded with a different codec, which would obviously involve a different plugin.
		*/
		virtual MediaCtrlFrame *encode(MediaCtrlFrame *outgoing) = 0;
		/**
		* @fn decode(MediaCtrlFrame *incoming)
		* Virtual method to decode a frame encoded with the codec
		* @param incoming The frame to decode
		* @returns The decoded frame if successful, NULL otherwise
		* @note The method returns a raw frame.
		*/
		virtual MediaCtrlFrame *decode(MediaCtrlFrame *incoming) = 0;
};

/// Type of frame
enum {
	/*! A normal frame, neither locking nor unlocking */
	NORMAL_FRAME = 0,
	/*! A locking frame, which would lock the channel it is sent on */
	LOCKING_FRAME,
	/*! An unlocking frame, which would unlock the channel it is sent on */
	UNLOCKING_FRAME,
};


/// Media Frame definition
/**
* @class MediaCtrlFrame MediaCtrlCodec.h
* The Frame (format+buffer+len) class.
* @note A frame instance must NEVER be directly destroyed with a delete: a dedicated collector frees old (3 seconds) frames automatically.
*/
class MediaCtrlFrame : public gc {
	public:
		/**
		* @fn MediaCtrlFrame(int media)
		* Default constructor. Just setups the default values.
		*/
		MediaCtrlFrame(int media=MEDIACTRL_MEDIA_AUDIO);
		/**
		* @fn MediaCtrlFrame(int media, uint8_t *buffer, int len, int format=MEDIACTRL_RAW)
		* Constructor. Setups a frame out of an existing buffer.
		* @param media The media type content (audio/video)
		* @param buffer The buffer
		* @param len The length (in bytes) of the buffer
		* @param format The format (optional) the buffer is encoded in (default is no encoding)
		*/
		MediaCtrlFrame(int media, uint8_t *buffer, int len, int format=MEDIACTRL_RAW);
		/**
		* @fn ~MediaCtrlFrame()
		* Destructor.
		*/
		~MediaCtrlFrame();

		/**
		* @fn setFormat(int format)
		* Sets the format of the frame buffer.
		* @param format The format of the frame buffer.
		*/
		void setFormat(int format) { this->format = format; };
		/**
		* @fn setBuffer(uint8_t *buffer, int len)
		* Sets the buffer for this frame.
		* @param buffer The buffer itself
		* @param len The length (in bytes) of the buffer
		*/
		void setBuffer(uint8_t *buffer, int len);
		/**
		* @fn setNormal()
		* Sets the frame as normal (i.e. neither locking nor unlocking).
		*/
		void setNormal() { type = NORMAL_FRAME; };
		/**
		* @fn setLocking()
		* Sets the frame as locking.
		*/
		void setLocking() { type = LOCKING_FRAME; };
		/**
		* @fn setUnlocking()
		* Sets the frame as unlocking.
		*/
		void setUnlocking() { type = UNLOCKING_FRAME; };
		/**
		* @fn setOwner(void *owner)
		* Sets the entity manipulating the frame (only needed in case of locked channels)
		* @param owner The entity manipulating the frame as an opaque pointer (e.g. a package dialog)
		*/
		void setOwner(void *owner) { this->owner = owner; };
		/**
		* @fn setTs(int ts)
		* Sets the timestamp increase provided by this frame.
		* @param ts The timestamp increase
		* @note Frames are forwarded with the same timestamp step as they arrived. */
		void setTs(int ts) { this->ts = ts; };
		/**
		* @fn setFlags(uint32_t flags)
		* Sets the flags mask associated with the frame.
		* @param flags The flags mask
		* @note The method does not check for existing flags, and so each call overwrites the previous one; it is up to the application to retrieve the previous flags mask by means of a getFlags() call and merge old and new flags
		*/
		void setFlags(uint32_t flags) { this->flags = flags; };
		/**
		* @fn setAllocated(bool allocated)
		* Sets if the passed buffer must be deallocated manually or not
		* @param allocated Whether to deallocate or not
		*/
		void setAllocated(bool allocated) { this->allocated = allocated; };
		/**
		* @fn setOriginal(MediaCtrlFrame *originalFrame)
		* In case this is a raw frame, the original (not decoded) frame can be kept for packages that might need it (e.g. passthrough mode)
		* @param originalFrame A pointer to the MediaCtrlFrame instance of the original undecoded frame
		*/
		void setOriginal(MediaCtrlFrame *originalFrame) { this->original = originalFrame; };
		/**
		* @fn setTransactionId(string tid)
		* Sets the Framework-level transaction identifier that originated this frame
		* @param tid A string addressing a valid transaction identifier
		*/
		void setTransactionId(string tid) { this->tid = tid; };

		/**
		* @fn getFormat()
		* Get the format of the frame.
		* @returns The format of the frame.
		*/
		int getFormat() { return format; };
		/**
		* @fn getMediaType()
		* Returns the frame media type
		* @returns The frame media type (MEDIACTRL_MEDIA_AUDIO)
		*/
		int getMediaType() { return media; };
		/**
		* @fn getBuffer()
		* Get the frame buffer.
		* @returns The frame buffer.
		*/
		uint8_t *getBuffer() { return buffer; };
		/**
		* @fn getLen()
		* Get the length of the frame buffer.
		* @returns The length of the frame buffer, -1 if no buffer is provided.
		*/
		int getLen() { return len; };
		/**
		* @fn getType()
		* Return the frame type.
		* @returns The frame type (e.g. LOCKING_FRAME if this is a channel locking frame).
		*/
		int getType() { return type; };
		/**
		* @fn getFlags()
		* Returns the flags mask associated with the frame.
		* @returns The flags mask
		*/
		uint32_t getFlags() { return flags; };

		/**
		* @fn appendFrame(MediaCtrlFrame *frame)
		* Appends a frame to the list of frames for this one: they all will have (if outgoing) or had (if incoming) the same timestamp
		* @param frame The frame to append
		* @note When marker bit is involved
		*/
		void appendFrame(MediaCtrlFrame *frame);
		/**
		* @fn getAppendedFrames()
		* Get the list of appended frames, i.e. the frames that have the same timestamp as this one
		* @returns The list of appended frame, if available
		* @note This list is only available if this frame is the first of a series, the others have no list...
		*/
		MediaCtrlFrames *getAppendedFrames() { return frames; };
		/**
		* @fn getOriginal()
		* Returns the original (not decoded) frame, in case it has been previously stored
		* @returns A pointer to the MediaCtrlFrame instance of the original undecoded frame
		*/
		MediaCtrlFrame *getOriginal() { return original; };
		/**
		* @fn getOwner()
		* Returns the pointer to the "owning" entity, in case a channel has been locked
		* @returns A pointer to the frame owner
		*/
		void *getOwner() { return owner; };
		/**
		* @fn getTransactionId()
		* Returns the Framework-level transaction identifier associated with the frame.
		* @returns The transaction identifier
		*/
		string getTransactionId() { return tid; };
		
		time_t getTimeBorn() { return timeBorn; };

		void setAllocator(int who) { this->who = who; };
		int getAllocator() { return who; };

	private:
		int media;		/*!< The media type of frame (audio) */
		int format;		/*!< The codec to use */
		uint8_t *buffer;	/*!< Buffer containing the frame sample */
		int len;		/*!< Length (in bytes) of the frame sample */
		int type;		/*!< After this frame, the channel may need to be (un)locked (e.g. type = LOCKING_FRAME) */
		uint32_t flags;		/*!< A flags mask for frame-related information */
		int counter;		/*!< Reference counter, keeping track of all users of this frame */
		int ts;			/*!< The timestamp step increase with respect to the previous frame */
		bool allocated;		/*!< If true, the buffer has been explicitely allocated, and so must be freed manually */
		time_t timeBorn;	/*!< When has the frame been allocated? (the collector uses this to free old frames) */

		MediaCtrlFrames *frames;	/*!< List of appended frames, which follow this frame with the same timestamp */

		MediaCtrlFrame *original;	/*!< The original, undecoded, frame, in case this is a raw frame */

		void *owner;		/*!< Opaque pointer only needed when locking/unlocking frames, and accessed by the RTP class consequently */
		int who;

		string tid;		/*!< Framework-level transaction identifier that originated this frame (needed for inter-package correlation) */
};

/*! @} */

}

#endif
