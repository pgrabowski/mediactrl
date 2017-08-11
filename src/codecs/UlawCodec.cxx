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
 * \brief mU-law Audio Codec (based on http://hazelware.luggle.com/tutorials/mulawcompression.html)
 *
 * \author Lorenzo Miniero <lorenzo.miniero@unina.it>
 *
 * \ingroup codecs
 * \ref codecs
 */

#include "MediaCtrlCodec.h"

using namespace std;
using namespace mediactrl;


#define ULAW_FRAME_LENGTH	160
#define RAW_BUFFER_LENGTH	8096


/// mU-law codec
/**
* @class UlawCodec UlawCodec.cxx
* The class, inheriting from MediaCtrlCodec, which implements the mU-law encoding and decoding features.
*/
class UlawCodec : public MediaCtrlCodec {
	public:
		/**
		* @fn UlawCodec()
		* Constructor. Just fills in the default stuff.
		* @note The codec is actually initialized only after a call to the start() member
		*/
		UlawCodec();
		/**
		* @fn ~UlawCodec()
		* Destructor.
		*/
		~UlawCodec();

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
		* Encodes a raw frame to mU-law.
		* @param outgoing A raw frame
		* @returns A mU-law encoded MediaCtrlFrame, if successful, NULL otherwise
		*/
		MediaCtrlFrame *encode(MediaCtrlFrame *outgoing);
		/**
		* @fn decode(MediaCtrlFrame *incoming)
		* Decodes a mU-law encoded frame to raw.
		* @param incoming The mU-law encoded frame
		* @returns The raw MediaCtrlFrame decoded frame, if successful, NULL otherwise
		*/
		MediaCtrlFrame *decode(MediaCtrlFrame *incoming);

		/**
		* @fn start()
		* This method actually initializes the codec functionality, making it ready to be used.
		*/
		bool start();
};


// UlawCodec Class Factories
extern "C" MediaCtrlCodec* create()
{
	UlawCodec *codec = new UlawCodec();
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


// Constants, tables and helpers
const int cBias = 0x84;
const int cClip = 32635;

static char MuLawCompressTable[256] =
{
	0,0,1,1,2,2,2,2,3,3,3,3,3,3,3,3,
	4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,
	5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,
	5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,
	6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,
	6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,
	6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,
	6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,
	7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,
	7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,
	7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,
	7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,
	7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,
	7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,
	7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,
	7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7
};

static short MuLawDecompressTable[256] =
{
     -32124,-31100,-30076,-29052,-28028,-27004,-25980,-24956,
     -23932,-22908,-21884,-20860,-19836,-18812,-17788,-16764,
     -15996,-15484,-14972,-14460,-13948,-13436,-12924,-12412,
     -11900,-11388,-10876,-10364, -9852, -9340, -8828, -8316,
      -7932, -7676, -7420, -7164, -6908, -6652, -6396, -6140,
      -5884, -5628, -5372, -5116, -4860, -4604, -4348, -4092,
      -3900, -3772, -3644, -3516, -3388, -3260, -3132, -3004,
      -2876, -2748, -2620, -2492, -2364, -2236, -2108, -1980,
      -1884, -1820, -1756, -1692, -1628, -1564, -1500, -1436,
      -1372, -1308, -1244, -1180, -1116, -1052,  -988,  -924,
       -876,  -844,  -812,  -780,  -748,  -716,  -684,  -652,
       -620,  -588,  -556,  -524,  -492,  -460,  -428,  -396,
       -372,  -356,  -340,  -324,  -308,  -292,  -276,  -260,
       -244,  -228,  -212,  -196,  -180,  -164,  -148,  -132,
       -120,  -112,  -104,   -96,   -88,   -80,   -72,   -64,
        -56,   -48,   -40,   -32,   -24,   -16,    -8,     0,
      32124, 31100, 30076, 29052, 28028, 27004, 25980, 24956,
      23932, 22908, 21884, 20860, 19836, 18812, 17788, 16764,
      15996, 15484, 14972, 14460, 13948, 13436, 12924, 12412,
      11900, 11388, 10876, 10364,  9852,  9340,  8828,  8316,
       7932,  7676,  7420,  7164,  6908,  6652,  6396,  6140,
       5884,  5628,  5372,  5116,  4860,  4604,  4348,  4092,
       3900,  3772,  3644,  3516,  3388,  3260,  3132,  3004,
       2876,  2748,  2620,  2492,  2364,  2236,  2108,  1980,
       1884,  1820,  1756,  1692,  1628,  1564,  1500,  1436,
       1372,  1308,  1244,  1180,  1116,  1052,   988,   924,
        876,   844,   812,   780,   748,   716,   684,   652,
        620,   588,   556,   524,   492,   460,   428,   396,
        372,   356,   340,   324,   308,   292,   276,   260,
        244,   228,   212,   196,   180,   164,   148,   132,
        120,   112,   104,    96,    88,    80,    72,    64,
         56,    48,    40,    32,    24,    16,     8,     0
};

uint8_t LinearToMuLawSample(short sample)
{
	int sign = (sample >> 8) & 0x80;
	if (sign)
		sample = (short)-sample;
	if (sample > cClip)
		sample = cClip;
	sample = (short)(sample + cBias);
	int exponent = (int)MuLawCompressTable[(sample>>7) & 0xFF];
	int mantissa = (sample >> (exponent+3)) & 0x0F;
	int compressedByte = ~ (sign | (exponent << 4) | mantissa);

	return (uint8_t)compressedByte;
}


// Class Methods
UlawCodec::UlawCodec()
{
	MCMINIT();
	cout << "[ULAW] Creating UlawCodec()" << endl;
	media = MEDIACTRL_MEDIA_AUDIO;
	format = MEDIACTRL_CODEC_ULAW;
	clockrate = 8000;
	name = nameMask = "PCMU";
	blockLen = ULAW_FRAME_LENGTH;
	avts.push_back(0);	// 0 is the static AVT payload format for U-law
	started = false;
}

UlawCodec::~UlawCodec()
{
	cout << "[ULAW] Removing UlawCodec()" << endl;
}

void UlawCodec::setCollector(void *frameCollector)
{
	setCommonCollector(frameCollector);
	cout << "[ULAW] Frame Collector " << (getCollector() ? "OK" : "NOT OK :(") << endl;
}

bool UlawCodec::start()
{
	started = true;
	// Nothing to be done here...
	return true;
}

bool UlawCodec::checkAvt(int avt)
{
	return(avt == avts.front());
}

MediaCtrlFrame *UlawCodec::encode(MediaCtrlFrame *outgoing)
{
	// FIXME are all these checks really useful?
	if(outgoing == NULL)
		return NULL;
	if((outgoing->getBuffer() == NULL) || (outgoing->getLen() < 0))
		return NULL;

	uint8_t *buffer = (uint8_t *)MCMALLOC(ULAW_FRAME_LENGTH, sizeof(uint8_t)), *tmp = buffer;
	if(!buffer)
		return NULL;

	int i=0;
	short *samples = (short*)outgoing->getBuffer();
	for(i = 0; i < ULAW_FRAME_LENGTH; i++)
		*tmp++ = LinearToMuLawSample(*samples++);

	MediaCtrlFrame *encoded = new MediaCtrlFrame();		// An U-law Frame
	encoded->setAllocator(CODEC);
	encoded->setFormat(MEDIACTRL_CODEC_ULAW);
	encoded->setBuffer((uint8_t *)buffer, ULAW_FRAME_LENGTH);
	MCMFREE(buffer);

	return encoded;
}

MediaCtrlFrame *UlawCodec::decode(MediaCtrlFrame *incoming)
{
	// FIXME are all these checks really useful?
	if(incoming == NULL)
		return NULL;
	if((incoming->getBuffer() == NULL) || (incoming->getLen() != ULAW_FRAME_LENGTH))
		return NULL;

	uint8_t *samples = incoming->getBuffer();
	short *buffer = (short *)MCMALLOC(ULAW_FRAME_LENGTH, sizeof(short)), *tmp = buffer;
	if(!buffer)
		return NULL;
	int i=0;
	for(i = 0; i < ULAW_FRAME_LENGTH; i++)
		*tmp++ = MuLawDecompressTable[*samples++];

	MediaCtrlFrame *decoded = new MediaCtrlFrame();		// A raw frame
	decoded->setAllocator(CODEC);
	decoded->setBuffer((uint8_t *)buffer, ULAW_FRAME_LENGTH*2);
	MCMFREE(buffer);

	return decoded;
}
