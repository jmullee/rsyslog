/* omfwd.c
 * This is the implementation of the build-in forwarding output module.
 *
 * NOTE: read comments in module-template.h to understand how this file
 *       works!
 *
 * File begun on 2007-07-20 by RGerhards (extracted from syslogd.c)
 * This file is under development and has not yet arrived at being fully
 * self-contained and a real object. So far, it is mostly an excerpt
 * of the "old" message code without any modifications. However, it
 * helps to have things at the right place one we go to the meat of it.
 *
 * Copyright 2007 Rainer Gerhards and Adiscon GmbH.
 *
 * This file is part of rsyslog.
 *
 * Rsyslog is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Rsyslog is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Rsyslog.  If not, see <http://www.gnu.org/licenses/>.
 *
 * A copy of the GPL can be found in the file "COPYING" in this distribution.
 */
#include "config.h"
#include "rsyslog.h"
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <netinet/in.h>
#include <netdb.h>
#include <fnmatch.h>
#include <assert.h>
#include <errno.h>
#include <ctype.h>
#include <unistd.h>
#ifdef USE_NETZIP
#include <zlib.h>
#endif
#include <pthread.h>
#include "syslogd.h"
#include "syslogd-types.h"
#include "srUtils.h"
#include "net.h"
#include "netstrms.h"
#include "netstrm.h"
#include "omfwd.h"
#include "template.h"
#include "msg.h"
#include "tcpclt.h"
#include "cfsysline.h"
#include "module-template.h"
#include "glbl.h"
#include "errmsg.h"

MODULE_TYPE_OUTPUT

/* internal structures
 */
DEF_OMOD_STATIC_DATA
DEFobjCurrIf(errmsg)
DEFobjCurrIf(glbl)
DEFobjCurrIf(net)
DEFobjCurrIf(netstrms)
DEFobjCurrIf(netstrm)
DEFobjCurrIf(tcpclt)

typedef struct _instanceData {
	netstrms_t *pNS; /* netstream subsystem */
	netstrm_t *pNetstrm; /* our output netstream */
	int iStrmDrvrMode;
	char	*f_hname;
	int *pSockArray;		/* sockets to use for UDP */
	int bIsConnected;  /* are we connected to remote host? 0 - no, 1 - yes, UDP means addr resolved */
	struct addrinfo *f_addr;
	int compressionLevel; /* 0 - no compression, else level for zlib */
	char *port;
	int protocol;
#	define	FORW_UDP 0
#	define	FORW_TCP 1
	/* following fields for TCP-based delivery */
	tcpclt_t *pTCPClt;	/* our tcpclt object */
} instanceData;

/* config data */
static uchar	*pszTplName = NULL; /* name of the default template to use */
int iStrmDrvrMode = 0; /* mode for stream driver, driver-dependent (0 mostly means plain tcp) */


/* get the syslog forward port from selector_t. The passed in
 * struct must be one that is setup for forwarding.
 * rgerhards, 2007-06-28
 * We may change the implementation to try to lookup the port
 * if it is unspecified. So far, we use the IANA default auf 514.
 */
static char *getFwdPt(instanceData *pData)
{
	assert(pData != NULL);
	if(pData->port == NULL)
		return("514");
	else
		return(pData->port);
}

BEGINcreateInstance
CODESTARTcreateInstance
ENDcreateInstance


BEGINisCompatibleWithFeature
CODESTARTisCompatibleWithFeature
	if(eFeat == sFEATURERepeatedMsgReduction)
		iRet = RS_RET_OK;
ENDisCompatibleWithFeature


BEGINfreeInstance
CODESTARTfreeInstance
	if(pData->f_addr != NULL) { /* TODO: is the check ok? */
		freeaddrinfo(pData->f_addr);
		pData->f_addr = NULL;
	}
	if(pData->port != NULL)
		free(pData->port);

	/* final cleanup */
	if(pData->pNetstrm != NULL)
		netstrm.Destruct(&pData->pNetstrm);
	if(pData->pSockArray != NULL)
		net.closeUDPListenSockets(pData->pSockArray);

	if(pData->protocol == FORW_TCP) {
		tcpclt.Destruct(&pData->pTCPClt);
	}

	if(pData->f_hname != NULL)
		free(pData->f_hname);

ENDfreeInstance


BEGINdbgPrintInstInfo
CODESTARTdbgPrintInstInfo
	printf("%s", pData->f_hname);
ENDdbgPrintInstInfo


/* Send a message via UDP
 * rgehards, 2007-12-20
 */
static rsRetVal UDPSend(instanceData *pData, char *msg, size_t len)
{
	DEFiRet;
	struct addrinfo *r;
	int i;
	unsigned lsent = 0;
	int bSendSuccess;

	if(pData->pSockArray != NULL) {
		/* we need to track if we have success sending to the remote
		 * peer. Success is indicated by at least one sendto() call
		 * succeeding. We track this be bSendSuccess. We can not simply
		 * rely on lsent, as a call might initially work, but a later
		 * call fails. Then, lsent has the error status, even though
		 * the sendto() succeeded. -- rgerhards, 2007-06-22
		 */
		bSendSuccess = FALSE;
		for (r = pData->f_addr; r; r = r->ai_next) {
			for (i = 0; i < *pData->pSockArray; i++) {
			       lsent = sendto(pData->pSockArray[i+1], msg, len, 0, r->ai_addr, r->ai_addrlen);
				if (lsent == len) {
					bSendSuccess = TRUE;
					break;
				} else {
					int eno = errno;
					char errStr[1024];
					dbgprintf("sendto() error: %d = %s.\n",
						eno, rs_strerror_r(eno, errStr, sizeof(errStr)));
				}
			}
			if (lsent == len && !send_to_all)
			       break;
		}
		/* finished looping */
		if (bSendSuccess == FALSE) {
			dbgprintf("error forwarding via udp, suspending\n");
			iRet = RS_RET_SUSPENDED;
		}
	}

	RETiRet;
}


/* CODE FOR SENDING TCP MESSAGES */


/* Send a frame via plain TCP protocol
 * rgerhards, 2007-12-28
 */
static rsRetVal TCPSendFrame(void *pvData, char *msg, size_t len)
{
	DEFiRet;
	ssize_t lenSend;
	instanceData *pData = (instanceData *) pvData;

	lenSend = len;
	CHKiRet(netstrm.Send(pData->pNetstrm, (uchar*)msg, &lenSend));
	dbgprintf("TCP sent %ld bytes, requested %ld\n", (long) lenSend, (long) len);

	if(lenSend != (ssize_t) len) {
		/* no real error, could "just" not send everything... 
		 * For the time being, we ignore this...
		 * rgerhards, 2005-10-25
		 */
		dbgprintf("message not completely (tcp)send, ignoring %ld\n", (long) lenSend);
		usleep(1000); /* experimental - might be benefitial in this situation */
		/* TODO: we need to revisit this code -- rgerhards, 2007-12-28 */
	}

finalize_it:
	RETiRet;
}


/* This function is called immediately before a send retry is attempted.
 * It shall clean up whatever makes sense.
 * rgerhards, 2007-12-28
 */
static rsRetVal TCPSendPrepRetry(void *pvData)
{
	DEFiRet;
	instanceData *pData = (instanceData *) pvData;

	assert(pData != NULL);
	netstrm.Destruct(&pData->pNetstrm);
	RETiRet;
}


/* initializes everything so that TCPSend can work.
 * rgerhards, 2007-12-28
 */
static rsRetVal TCPSendInit(void *pvData)
{
	DEFiRet;
	instanceData *pData = (instanceData *) pvData;

	assert(pData != NULL);
	if(pData->pNetstrm == NULL) {
		CHKiRet(netstrms.Construct(&pData->pNS));
		/* here we may set another netstream driver (e.g. to do TLS) */
		CHKiRet(netstrms.ConstructFinalize(pData->pNS));

		/* now create the actual stream and connect to the server */
		CHKiRet(netstrms.CreateStrm(pData->pNS, &pData->pNetstrm));
		CHKiRet(netstrm.ConstructFinalize(pData->pNetstrm));
		CHKiRet(netstrm.SetDrvrMode(pData->pNetstrm, pData->iStrmDrvrMode));
		CHKiRet(netstrm.Connect(pData->pNetstrm, glbl.GetDefPFFamily(),
			(uchar*)pData->port, (uchar*)pData->f_hname));
	}

finalize_it:
	if(iRet != RS_RET_OK) {
		if(pData->pNetstrm != NULL)
			netstrm.Destruct(&pData->pNetstrm);
		if(pData->pNS != NULL)
			netstrms.Destruct(&pData->pNS);
	}

	RETiRet;
}


/* try to resume connection if it is not ready
 * rgerhards, 2007-08-02
 */
static rsRetVal doTryResume(instanceData *pData)
{
	DEFiRet;
	struct addrinfo *res;
	struct addrinfo hints;

	if(pData->bIsConnected)
		FINALIZE;

	/* The remote address is not yet known and needs to be obtained */
	dbgprintf(" %s\n", pData->f_hname);
	if(pData->protocol == FORW_UDP) {
		memset(&hints, 0, sizeof(hints));
		/* port must be numeric, because config file syntax requires this */
		hints.ai_flags = AI_NUMERICSERV;
		hints.ai_family = glbl.GetDefPFFamily();
		hints.ai_socktype = pData->protocol == SOCK_DGRAM;
		if((getaddrinfo(pData->f_hname, getFwdPt(pData), &hints, &res)) != 0) {
			ABORT_FINALIZE(RS_RET_SUSPENDED);
		}
		dbgprintf("%s found, resuming.\n", pData->f_hname);
		pData->f_addr = res;
		pData->bIsConnected = 1;
		if(pData->pSockArray == NULL) {
			pData->pSockArray = net.create_udp_socket((uchar*)pData->f_hname, NULL, 0);
		}
	} else {
		CHKiRet(TCPSendInit((void*)pData));
	}

finalize_it:
	if(iRet != RS_RET_OK) {
		if(pData->f_addr != NULL) {
			freeaddrinfo(pData->f_addr);
			pData->f_addr = NULL;
		}
		iRet = RS_RET_SUSPENDED;
	}

	RETiRet;
}


BEGINtryResume
CODESTARTtryResume
	iRet = doTryResume(pData);
ENDtryResume

BEGINdoAction
	char *psz; /* temporary buffering */
	register unsigned l;
CODESTARTdoAction
	CHKiRet(doTryResume(pData));

	dbgprintf(" %s:%s/%s\n", pData->f_hname, getFwdPt(pData),
		 pData->protocol == FORW_UDP ? "udp" : "tcp");

	psz = (char*) ppString[0];
	l = strlen((char*) psz);
	if (l > MAXLINE)
		l = MAXLINE;

#	ifdef	USE_NETZIP
	/* Check if we should compress and, if so, do it. We also
	 * check if the message is large enough to justify compression.
	 * The smaller the message, the less likely is a gain in compression.
	 * To save CPU cycles, we do not try to compress very small messages.
	 * What "very small" means needs to be configured. Currently, it is
	 * hard-coded but this may be changed to a config parameter.
	 * rgerhards, 2006-11-30
	 */
	if(pData->compressionLevel && (l > MIN_SIZE_FOR_COMPRESS)) {
		Bytef out[MAXLINE+MAXLINE/100+12] = "z";
		uLongf destLen = sizeof(out) / sizeof(Bytef);
		uLong srcLen = l;
		int ret;
		ret = compress2((Bytef*) out+1, &destLen, (Bytef*) psz,
				srcLen, pData->compressionLevel);
		dbgprintf("Compressing message, length was %d now %d, return state  %d.\n",
			l, (int) destLen, ret);
		if(ret != Z_OK) {
			/* if we fail, we complain, but only in debug mode
			 * Otherwise, we are silent. In any case, we ignore the
			 * failed compression and just sent the uncompressed
			 * data, which is still valid. So this is probably the
			 * best course of action.
			 * rgerhards, 2006-11-30
			 */
			dbgprintf("Compression failed, sending uncompressed message\n");
		} else if(destLen+1 < l) {
			/* only use compression if there is a gain in using it! */
			dbgprintf("there is gain in compression, so we do it\n");
			psz = (char*) out;
			l = destLen + 1; /* take care for the "z" at message start! */
		}
		++destLen;
	}
#	endif

	if(pData->protocol == FORW_UDP) {
		/* forward via UDP */
		CHKiRet(UDPSend(pData, psz, l));
	} else {
		/* forward via TCP */
		rsRetVal ret;
		ret = tcpclt.Send(pData->pTCPClt, pData, psz, l);
		if(ret != RS_RET_OK) {
			/* error! */
			dbgprintf("error forwarding via tcp, suspending\n");
			iRet = RS_RET_SUSPENDED;
		}
	}
finalize_it:
ENDdoAction


/* This function loads TCP support, if not already loaded. It will be called
 * during config processing. To server ressources, TCP support will only
 * be loaded if it actually is used. -- rgerhard, 2008-04-17
 */
static rsRetVal
loadTCPSupport(void)
{
	DEFiRet;
	if(!netstrms.ifIsLoaded)
		CHKiRet(objUse(netstrms, LM_NETSTRMS_FILENAME));
	if(!netstrm.ifIsLoaded)
		CHKiRet(objUse(netstrm, LM_NETSTRM_FILENAME));
	if(!tcpclt.ifIsLoaded)
		CHKiRet(objUse(tcpclt, LM_TCPCLT_FILENAME));

finalize_it:
	RETiRet;
}


BEGINparseSelectorAct
	uchar *q;
	int i;
	int bErr;
	rsRetVal localRet;
        struct addrinfo;
	TCPFRAMINGMODE tcp_framing = TCP_FRAMING_OCTET_STUFFING;
CODESTARTparseSelectorAct
CODE_STD_STRING_REQUESTparseSelectorAct(1)
	if(*p != '@')
		ABORT_FINALIZE(RS_RET_CONFLINE_UNPROCESSED);

	CHKiRet(createInstance(&pData));

	++p; /* eat '@' */
	if(*p == '@') { /* indicator for TCP! */
		localRet = loadTCPSupport();
		if(localRet != RS_RET_OK) {
			errmsg.LogError(NO_ERRCODE, "could not activate network stream modules for TCP "
					"(internal error %d) - are modules missing?", localRet);
			ABORT_FINALIZE(localRet);
		}
		pData->protocol = FORW_TCP;
		++p; /* eat this '@', too */
	} else {
		pData->protocol = FORW_UDP;
	}
	/* we are now after the protocol indicator. Now check if we should
	 * use compression. We begin to use a new option format for this:
	 * @(option,option)host:port
	 * The first option defined is "z[0..9]" where the digit indicates
	 * the compression level. If it is not given, 9 (best compression) is
	 * assumed. An example action statement might be:
	 * @@(z5,o)127.0.0.1:1400  
	 * Which means send via TCP with medium (5) compresion (z) to the local
	 * host on port 1400. The '0' option means that octet-couting (as in
	 * IETF I-D syslog-transport-tls) is to be used for framing (this option
	 * applies to TCP-based syslog only and is ignored when specified with UDP).
	 * That is not yet implemented.
	 * rgerhards, 2006-12-07
	 */
	if(*p == '(') {
		/* at this position, it *must* be an option indicator */
		do {
			++p; /* eat '(' or ',' (depending on when called) */
			/* check options */
			if(*p == 'z') { /* compression */
#				ifdef USE_NETZIP
				++p; /* eat */
				if(isdigit((int) *p)) {
					int iLevel;
					iLevel = *p - '0';
					++p; /* eat */
					pData->compressionLevel = iLevel;
				} else {
					errmsg.LogError(NO_ERRCODE, "Invalid compression level '%c' specified in "
						 "forwardig action - NOT turning on compression.",
						 *p);
				}
#				else
				errmsg.LogError(NO_ERRCODE, "Compression requested, but rsyslogd is not compiled "
					 "with compression support - request ignored.");
#				endif /* #ifdef USE_NETZIP */
			} else if(*p == 'o') { /* octet-couting based TCP framing? */
				++p; /* eat */
				/* no further options settable */
				tcp_framing = TCP_FRAMING_OCTET_COUNTING;
			} else { /* invalid option! Just skip it... */
				errmsg.LogError(NO_ERRCODE, "Invalid option %c in forwarding action - ignoring.", *p);
				++p; /* eat invalid option */
			}
			/* the option processing is done. We now do a generic skip
			 * to either the next option or the end of the option
			 * block.
			 */
			while(*p && *p != ')' && *p != ',')
				++p;	/* just skip it */
		} while(*p && *p == ','); /* Attention: do.. while() */
		if(*p == ')')
			++p; /* eat terminator, on to next */
		else
			/* we probably have end of string - leave it for the rest
			 * of the code to handle it (but warn the user)
			 */
			errmsg.LogError(NO_ERRCODE, "Option block not terminated in forwarding action.");
	}
	/* extract the host first (we do a trick - we replace the ';' or ':' with a '\0')
	 * now skip to port and then template name. rgerhards 2005-07-06
	 */
	for(q = p ; *p && *p != ';' && *p != ':' ; ++p)
		/* JUST SKIP */;

	pData->port = NULL;
	if(*p == ':') { /* process port */
		uchar * tmp;

		*p = '\0'; /* trick to obtain hostname (later)! */
		tmp = ++p;
		for(i=0 ; *p && isdigit((int) *p) ; ++p, ++i)
			/* SKIP AND COUNT */;
		pData->port = malloc(i + 1);
		if(pData->port == NULL) {
			errmsg.LogError(NO_ERRCODE, "Could not get memory to store syslog forwarding port, "
				 "using default port, results may not be what you intend\n");
			/* we leave f_forw.port set to NULL, this is then handled by getFwdPt(). */
		} else {
			memcpy(pData->port, tmp, i);
			*(pData->port + i) = '\0';
		}
	}
	
	/* now skip to template */
	bErr = 0;
	while(*p && *p != ';') {
		if(*p && *p != ';' && !isspace((int) *p)) {
			if(bErr == 0) { /* only 1 error msg! */
				bErr = 1;
				errno = 0;
				errmsg.LogError(NO_ERRCODE, "invalid selector line (port), probably not doing "
					 "what was intended");
			}
		}
		++p;
	}

	/* TODO: make this if go away! */
	if(*p == ';') {
		*p = '\0'; /* trick to obtain hostname (later)! */
		CHKmalloc(pData->f_hname = strdup((char*) q));
		*p = ';';
	} else {
		CHKmalloc(pData->f_hname = strdup((char*) q));
	}

	/* process template */
	CHKiRet(cflineParseTemplateName(&p, *ppOMSR, 0, OMSR_NO_RQD_TPL_OPTS,
			(pszTplName == NULL) ? (uchar*)"RSYSLOG_TraditionalForwardFormat" : pszTplName));

	if(pData->protocol == FORW_TCP) {
		/* create our tcpclt */
		CHKiRet(tcpclt.Construct(&pData->pTCPClt));
		/* and set callbacks */
		CHKiRet(tcpclt.SetSendInit(pData->pTCPClt, TCPSendInit));
		CHKiRet(tcpclt.SetSendFrame(pData->pTCPClt, TCPSendFrame));
		CHKiRet(tcpclt.SetSendPrepRetry(pData->pTCPClt, TCPSendPrepRetry));
		CHKiRet(tcpclt.SetFraming(pData->pTCPClt, tcp_framing));
		pData->iStrmDrvrMode = iStrmDrvrMode;
	}

CODE_STD_FINALIZERparseSelectorAct
ENDparseSelectorAct


BEGINmodExit
CODESTARTmodExit
	/* release what we no longer need */
	objRelease(errmsg, CORE_COMPONENT);
	objRelease(glbl, CORE_COMPONENT);
	objRelease(net, LM_NET_FILENAME);
	if(netstrm.ifIsLoaded)
		objRelease(netstrm, LM_NETSTRM_FILENAME);
	if(netstrms.ifIsLoaded)
		objRelease(netstrms, LM_NETSTRMS_FILENAME);
	if(!tcpclt.ifIsLoaded)
		objRelease(tcpclt, LM_TCPCLT_FILENAME);

	if(pszTplName != NULL) {
		free(pszTplName);
		pszTplName = NULL;
	}
ENDmodExit


BEGINqueryEtryPt
CODESTARTqueryEtryPt
CODEqueryEtryPt_STD_OMOD_QUERIES
ENDqueryEtryPt


/* Reset config variables for this module to default values.
 * rgerhards, 2008-03-28
 */
static rsRetVal resetConfigVariables(uchar __attribute__((unused)) *pp, void __attribute__((unused)) *pVal)
{
	if(pszTplName != NULL) {
		free(pszTplName);
		pszTplName = NULL;
	}
	iStrmDrvrMode = 0;

	return RS_RET_OK;
}


BEGINmodInit(Fwd)
CODESTARTmodInit
	*ipIFVersProvided = CURR_MOD_IF_VERSION; /* we only support the current interface specification */
CODEmodInit_QueryRegCFSLineHdlr
	CHKiRet(objUse(glbl, CORE_COMPONENT));
	CHKiRet(objUse(errmsg, CORE_COMPONENT));
	CHKiRet(objUse(net,LM_NET_FILENAME));

	CHKiRet(regCfSysLineHdlr((uchar *)"actionforwarddefaulttemplate", 0, eCmdHdlrGetWord, NULL, &pszTplName, NULL));
	CHKiRet(regCfSysLineHdlr((uchar *)"actionsendstreamdrivermode", 0, eCmdHdlrInt, NULL, &iStrmDrvrMode, NULL));
	CHKiRet(omsdRegCFSLineHdlr((uchar *)"resetconfigvariables", 1, eCmdHdlrCustomHandler, resetConfigVariables, NULL, STD_LOADABLE_MODULE_ID));
ENDmodInit

/* vim:set ai:
 */
