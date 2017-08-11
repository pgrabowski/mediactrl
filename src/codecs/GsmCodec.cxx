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

/*! \file
 *
 * \brief GSM Audio Codec (based on libgsm)
 *
 * \author Lorenzo Miniero <lorenzo.miniero@unina.it>
 *
 * \ingroup codecs
 * \ref codecs
 */

extern "C" {
#include <gsm.h>	/* libgsm */
}

#include "MediaCtrlCodec.h"

using namespace std;
using namespace mediactrl;


#define GSM_FRAME_LENGTH	33
#define GSM_SAMPLES		160


/// GSM codec
/**
* @class GsmCodec GsmCodec.cxx
* The class, inheriting from MediaCtrlCodec, which implements the GSM encoding and decoding features.
*/
class GsmCodec : public MediaCtrlCodec {
	public:
		/**
		* @fn GsmCodec()
		* Constructor. Just fills in the default stuff.
		* @note The codec is actually initialized only after a call to the start() member
		*/
		GsmCodec();
		/**
		* @fn ~GsmCodec()
		* Destructor.
		*/
		~GsmCodec();

		void setCollector(void *frameCollector);

		/**
		* @fn checkAvt(int avt)
		* This method checks if the provided AVT profile identifier is valid for this codec.
		* @param avt A numeric AVT profile identifier
		* @returns true if it is, false otherwise
		*/
		bool checkAvt(int avt);

		/**
		* @fn addSetting(string var, string value)
		* Method to pass to the codec some specific settings before startup (unused)
		* @param var The name of the setting (ignored)
		* @param value The value(s) for the setting (ignored)
		*/
		void addSetting(string var, string value) { return; };

		/**
		* @fn encode(MediaCtrlFrame *outgoing)
		* Encodes a raw frame to GSM.
		* @param outgoing A raw frame
		* @returns A GSM encoded MediaCtrlFrame, if successful, NULL otherwise
		*/
		MediaCtrlFrame *encode(MediaCtrlFrame *outgoing);
		/**
		* @fn decode(MediaCtrlFrame *incoming)
		* Decodes a GSM encoded frame to raw.
		* @param incoming The GSM encoded frame
		* @returns The raw MediaCtrlFrame decoded frame, if successful, NULL otherwise
		*/
		MediaCtrlFrame *decode(MediaCtrlFrame *incoming);

		/**
		* @fn start()
		* This method actually initializes the codec functionality, making it ready to be used.
		*/
		bool start();

	private:
		gsm codec;	// The libgsm encoding/decoding context
};


// GsmCodec Class Factories
extern "C" MediaCtrlCodec* create()
{
	GsmCodec *codec = new GsmCodec();
	return codec;
}

extern "C" void destroy(MediaCtrlCodec* c)
{
	delete c;
}

extern "C" void purge()
{
	// Nothing to do here
}

// Class Methods
GsmCodec::GsmCodec()
{
	MCMINIT();
	cout << "[GSM] Creating GsmCodec()" << endl;
	media = MEDIACTRL_MEDIA_AUDIO;
	format = MEDIACTRL_CODEC_GSM;
	clockrate = 8000;
	name = nameMask = "GSM";
	blockLen = GSM_FRAME_LENGTH;
	avts.push_back(3);	// 3 is the static AVT payload format for GSM
	codec = NULL;
	started = false;
}

GsmCodec::~GsmCodec()
{
	cout << "[GSM] Removing GsmCodec()" << endl;
	if(codec)
		gsm_destroy(codec);
}

void GsmCodec::setCollector(void *frameCollector)
{
	setCommonCollector(frameCollector);
	cout << "[GSM] Frame Collector " << (getCollector() ? "OK" : "NOT OK :(") << endl;
}

bool GsmCodec::start()
{
	if(codec)	// This is a restart
		gsm_destroy(codec);

	codec = gsm_create();
	if(!codec) {
		cout << "[GSM] Error creating GSM decoder/encoder" << endl;
		return false;
	}
	started = true;
	return true;
}

bool GsmCodec::checkAvt(int avt)
{
	return(avt == avts.front());
}

MediaCtrlFrame *GsmCodec::encode(MediaCtrlFrame *outgoing)
{
	// TODO Handle chunking
	// FIXME are all these checks really useful?
	if(outgoing == NULL)
		return NULL;
	if((outgoing->getBuffer() == NULL) || (outgoing->getLen() != (GSM_SAMPLES*2))) {
		cout << "[GSM] Invalid buffer: " << outgoing->getLen() << "!=" << dec << (GSM_SAMPLES*2) << endl;
		return NULL;
	}

	gsm_byte *buffer = (gsm_byte *)MCMALLOC(GSM_FRAME_LENGTH, sizeof(gsm_byte));
	if(!buffer)
		return NULL;
	gsm_encode(codec, (gsm_signal *)outgoing->getBuffer(), buffer);

	MediaCtrlFrame *encoded = new MediaCtrlFrame();		// A GSM Frame
	encoded->setAllocator(CODEC);
	encoded->setFormat(MEDIACTRL_CODEC_GSM);
	encoded->setBuffer((uint8_t *)buffer, GSM_FRAME_LENGTH);
	MCMFREE(buffer);

	return encoded;
}

MediaCtrlFrame *GsmCodec::decode(MediaCtrlFrame *incoming)
{
	// FIXME are all these checks really useful?
	if(incoming == NULL)
		return NULL;
	if((incoming->getBuffer() == NULL) || (incoming->getLen() != GSM_FRAME_LENGTH))
		return NULL;	// FIXME how to handle MSGSM?

	gsm_signal *buffer = (gsm_signal *)MCMALLOC(GSM_SAMPLES, sizeof(gsm_signal));
	if(!buffer)
		return NULL;
	if(gsm_decode(codec, (gsm_byte *)incoming->getBuffer(), buffer) < 0)
		return NULL;

	MediaCtrlFrame *decoded = new MediaCtrlFrame();		// A raw frame
	decoded->setAllocator(CODEC);
	decoded->setBuffer((uint8_t *)buffer, GSM_SAMPLES*2);
	MCMFREE(buffer);

	return decoded;
}
