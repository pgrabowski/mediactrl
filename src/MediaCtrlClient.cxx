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
 * \brief CFW Client Information Handler
 *
 * \author Lorenzo Miniero <lorenzo.miniero@unina.it>
 *
 * \ingroup core
 * \ref core
 */

#include "MediaCtrlClient.h"
#include <errno.h>

#include <openssl/rsa.h>
#include <openssl/crypto.h>
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/x509.h>
#include <openssl/pem.h>


using namespace mediactrl;
using namespace ost;

static TlsSetup *tls;
static SSL_CTX *context = NULL;
static SSL_METHOD *method = NULL;
static int verify_callback(int preverify_ok, X509_STORE_CTX *ctx);
string sha1Fingerprint(SSL *session);
string sha1FingerprintPeer(SSL *session);


int closeFd(int fd)
{
	return close(fd);
}

static int verify_callback(int preverify_ok, X509_STORE_CTX *ctx)
{
	// We just use the verify_callback to request a certificate from the client
	return 1;
}

string sha1Fingerprint(SSL *session)
{
	if(!session)
		return "";
	const char hexcodes[] = "0123456789ABCDEF";
	char fingerprint[EVP_MAX_MD_SIZE * 3];
	unsigned char md[EVP_MAX_MD_SIZE];
	unsigned int md_size = 0, i = 0;
	if(X509_digest(SSL_get_certificate(session), EVP_sha1(), md, &md_size)) {
		for (i=0; i<md_size; i++) {
			fingerprint[3*i] = hexcodes[(md[i] & 0xf0) >> 4];
			fingerprint[(3*i)+1] = hexcodes[(md[i] & 0x0f)];
			fingerprint[(3*i)+2] = ':';
		}
		if (md_size > 0)
			fingerprint[(3*(md_size-1))+2] = '\0';
		else
			fingerprint[0] = '\0';
		return fingerprint;
	}
	return "";
}

string sha1FingerprintPeer(SSL *session)
{
	if(!session)
		return "";
	const char hexcodes[] = "0123456789ABCDEF";
	char fingerprint[EVP_MAX_MD_SIZE * 3];
	unsigned char md[EVP_MAX_MD_SIZE];
	unsigned int md_size = 0, i = 0;
	if(X509_digest(SSL_get_peer_certificate(session), EVP_sha1(), md, &md_size)) {
		for (i=0; i<md_size; i++) {
			fingerprint[3*i] = hexcodes[(md[i] & 0xf0) >> 4];
			fingerprint[(3*i)+1] = hexcodes[(md[i] & 0x0f)];
			fingerprint[(3*i)+2] = ':';
		}
		if (md_size > 0)
			fingerprint[(3*(md_size-1))+2] = '\0';
		else
			fingerprint[0] = '\0';
		return fingerprint;
	}
	return "";
}


bool TlsSetup::setup(string certificate, string privatekey)
{
	cout << "[MSCC] Enabling TLS" << endl;
	if((certificate == "") || (privatekey == "")) {
		cout << "[MSCC] \tInvalid certificate/privatekey" << endl;
		return false;
	}
	if((context != NULL) && (method != NULL)) {
		cout << "[MSCC] \tTLS already setup" << endl;
		return false;
	}
	this->certificate = certificate;
	this->privatekey = privatekey;
	cout << "[MSCC] \tCertificate = " << certificate << endl;
	cout << "[MSCC] \tPrivate Key = " << privatekey << endl;
	SSL_library_init();
	SSL_load_error_strings();
	method = TLSv1_server_method();
	if(!method) {
		cout << "[MSCC] \tTLSv1_server_method failed" << endl;
		return false;
	}
	context = SSL_CTX_new(method);
	if(!context) {
		cout << "[MSCC] \tSSL_CTX_new failed" << endl;
		return false;
	}
	if(SSL_CTX_use_certificate_file(context, certificate.c_str(), SSL_FILETYPE_PEM) < 1) {
		cout << "[MSCC] \tSSL_CTX_use_certificate_file (invalid file? '" << certificate << "')" << endl;
		return false;
	}
	if(SSL_CTX_use_PrivateKey_file(context, privatekey.c_str(), SSL_FILETYPE_PEM) < 1) {
		cout << "[MSCC] \tSSL_CTX_use_PrivateKey_file (invalid file? '" << privatekey << "')" << endl;
		return false;
	}
	if(!SSL_CTX_check_private_key(context)) {
		cout << "[MSCC] \tSSL_CTX_check_private_key failed" << endl;
		return false;
	}
	if(SSL_CTX_set_cipher_list(context, SSL_DEFAULT_CIPHER_LIST) < 0) {
		cout << "[MSCC] \tSSL_CTX_set_cipher_list failed" << endl;
		return false;
	}
	SSL* session = SSL_new(context);
	if(!session) {
		cout << "[MSCC] \tTest SSL_new failed" << endl;
		return false;
	}
	// Get the SHA-1 fingerprint and store it
	fingerprint = sha1Fingerprint(session);
	SSL_free(session);
	cout << "[MSCC] SHA-1: " << (fingerprint != "" ? fingerprint : "???") << endl;

	return true;
}


MediaCtrlClient::MediaCtrlClient(MediaCtrlClientCallback *callback, bool tls, string fingerprint)
{
	fd = -1;
	authenticated = false;
	timer = NULL;
	this->callback = callback;
	alive = false;
	cond = new ost::Conditional();
	pollfds.fd = -1;
	pollfds.events = 0;
	pollfds.revents = 0;
	this->tls = tls;
	this->fingerprint = fingerprint;
	accepted = false;
	session = NULL;
}

MediaCtrlClient::~MediaCtrlClient()
{
	if(timer) {
		delete timer;
		timer = NULL;
	}
	alive = false;
	cout << "[MSCC] Erasing Dialog-id: " << dialogId << endl;
	cout << "[MSCC] Closing associated connection (fd=" << dec << fd << ")" << endl;
	shutdown(fd, SHUT_RDWR);
	closeFd(fd);
	setFd(-1);
	cond->signal(true);
	delete cond;
	cond = NULL;
}

void MediaCtrlClient::run()
{
	cout << "[MSCC] Thread starting: " << dialogId << endl;

	int err = 0, i = 0, trailer = 0, position = 0;
	char buffer;
	int msglen = 100;
	char *message = (char *)MCMALLOC(msglen, sizeof(char)), *tmpmsg = NULL;
	bool ok = false;

	alive = true;

	while(alive) {
		if(fd < 0) {
			// Wait condition
			cout << "[MSCC] Waiting for a valid fd..." << endl;
			cond->wait();
			if(fd > -1) {
				cout << "[MSCC] Using fd=" << dec << fd << endl;
				pollfds.fd = fd;
				pollfds.events = POLLIN;
				pollfds.revents = 0;
			}
		}
		if(!alive)
			break;
		while((err = poll(&pollfds, 1, 1000) < 0) && (errno == EINTR));
		if(err < 0)
			continue;	// Poll error, FIXME
		else {
			// Check which descriptor has data here ...
			if(pollfds.revents == POLLIN) {
				if(pollfds.fd == fd) {	// We have a new message from this client
					// Start from the header, and pass it to the stack
					ok = false;
					trailer = 0;
					position = 0;
					while(1) {
						if(!tls)
							err = recv(fd, &buffer, 1, 0);
						else {
							if(!session)	// We can't do anything until we get a session
								continue;
							if(accepted) {
								err = SSL_read((SSL*)session, &buffer, 1);
							} else {
								cout << "[MSCC] Trying to accept SSL connection..." << endl;
								if(SSL_accept((SSL*)session) < 1) {
									cout << "[MSCC] \tSSL_accept failed" << endl;
									callback->connectionLost(fd);
									shutdown(fd, SHUT_RDWR);
									closeFd(fd);
									setFd(-1);
									break;
								} else {
									// Check if the fingerprint matches
									string sha1 = sha1FingerprintPeer((SSL*)session);
									if(sha1 != fingerprint) {
										cout << "[MSCC] \tFingerprints don't match!" << endl;
										cout << "[MSCC] \t\tNegotiated: " << fingerprint << endl;
										cout << "[MSCC] \t\tConnected:  " << sha1 << endl;
										callback->connectionLost(fd);
										shutdown(fd, SHUT_RDWR);
										closeFd(fd);
										setFd(-1);
										SSL_free((SSL*)session);
										session = NULL;
										break;										
									}
									cout << "[MSCC] \tFingerprints successfully matched" << endl;
									cout << "[MSCC] \t\tNegotiated: " << fingerprint << endl;
									cout << "[MSCC] \t\tConnected:  " << sha1 << endl;
									accepted = true;
									continue;
								}
							}
						}
						if(err < 1) {
							ok = false;
							cout << "[MSCC] Lost connection to fd=" << fd << endl;
							callback->connectionLost(fd);
							setFd(-1);
							break;
						} else {
							ok = true;
							memcpy(message + position, &buffer, err);
							position = position + err;
							memset(message + position, '\0', 1);
							if((msglen-position) < 10) {
								msglen += 50;
								tmpmsg = (char *)MCMREALLOC(message, msglen);
								if(!tmpmsg) {
									ok = false;	// FIXME
									break;
								}
								message = tmpmsg;
							}
							switch(trailer) {
								case 1:
								case 3:
									if(buffer == '\n')
										trailer++;
									else
										trailer = 0;
									break;
								case 0:
								case 2:
									if(buffer == '\r')
										trailer++;
									else
										trailer = 0;
									break;
								default:
									trailer = 0;
									break;
							}
							if(trailer == 4)	// Found the "\r\n\r\n"
								break;
						}
					}
					if(ok && (err > 0)) {
						string newMessage = (const char *)message;
						callback->parseMessage(this, newMessage);
					}
				}
			}
		}
	}
	MCMFREE(message);
	message = NULL;

	cout << "[MSCC] Thread leaving: " << dialogId << endl;
}

int MediaCtrlClient::sendMessage(string text)
{
	if(fd < 0)
		return -1;
	if(text == "")
		return -1;
	if(!tls)
		return send(fd, text.c_str(), text.length(), 0);		// FIXME
	else {
		if(session)
			return SSL_write((SSL*)session, text.c_str(), text.length());
	}
	return -1;
}

string MediaCtrlClient::getContent(MediaCtrlClientCallback *callback, int len)
{
	if(this->callback != callback) {
		cout << "[MSCC] Invalid CFW Stack" << endl;
		return "";
	}
	if(fd < 0)
		return "";
	char *blob = (char *)MCMALLOC(len+1, sizeof(char));
	int total = 0, err = 0;
	while(1) {
		if(!tls)
			err = recv(fd, blob+total, len-total, 0);
		else {
			if(session)
				err = SSL_read((SSL*)session, blob+total, len-total);
			else
				err = 0;
		}
		if(err < 1) {
			// TODO Handle error
		} else {
			total += err;
			if(total >= len)
				break;
		}
	}
	string result = blob;
	MCMFREE(blob);
	blob = NULL;
	return result;
}

void MediaCtrlClient::setDialog(string callId, string cfwId)
{
	this->callId = callId;
	dialogId = cfwId;
	cout << "[MSCC] Dialog-id: " << dialogId << endl;
}

void MediaCtrlClient::setAddress(string ip, uint16_t port)
{
	this->ip = ip;
	this->port = port;
	cout << "[MSCC] Address: " << ip << ":" << port << endl;
}

void MediaCtrlClient::setFd(int fd)
{
	if(fd == -1) {
		this->fd = fd;
		pollfds.fd = -1;
		pollfds.events = 0;
		pollfds.revents = 0;
		if(tls && session)
			SSL_free((SSL*)session);
		session = NULL;
		accepted = false;
		return;
	}
	if(!tls)
		this->fd = fd;
	else {
		if(session)
			SSL_free((SSL*)session);
		session = NULL;
		accepted = false;
		SSL* sslSession = SSL_new(context);
		session = sslSession;
		if(session) {
			// Explicitly request the peer to send its certificate
			SSL_set_verify((SSL*)session, SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT, verify_callback);
			SSL_set_verify_depth((SSL*)session, 1);
			// Finally associate the file descriptor with the SSL session
			if(SSL_set_fd((SSL*)session, fd) < 1)
				cout << "[MSCC] Couldn't associated fd=" << dec << fd << " with SSL session..." << endl;
			else
				cout << "[MSCC] Successfully associated fd=" << dec << fd << " with SSL session" << endl;
		}
		this->fd = fd;
	}
	cond->signal(true);
	keepAlive = 100000;	// By default we'll wait 5 seconds for a SYNCH
	startCounter();
}

void MediaCtrlClient::setKeepAlive(int seconds)
{
	cout << "[MSCC] Keep-Alive (re)set to " << dec << seconds << " seconds" << endl;
	keepAlive = seconds*1000;
	resetCounter();
}

void MediaCtrlClient::startCounter()
{
	cout << "[MSCC] Counter started" << endl;
	timer = new TimerPort();
	timer->incTimer(0);
	resetCounter();
}

void MediaCtrlClient::resetCounter()
{
	if(timer)
		startTime = timer->getElapsed();
}

bool MediaCtrlClient::getTimeout()
{
	if(!timer)
		return false;
	if((timer->getElapsed() - startTime) > keepAlive)
		return true;
	return false;
}

string MediaCtrlClient::getIp()
{
	struct sockaddr_in address;
	if(inet_aton(ip.c_str(), (in_addr *)&address) == 1)
		return ip;
	struct hostent *host = gethostbyname(ip.c_str());
	if(host == NULL)
		return ip;	// FIXME
	address.sin_addr.s_addr = *(uint32_t *)(host->h_addr_list[0]);
	return inet_ntoa(address.sin_addr);
}
