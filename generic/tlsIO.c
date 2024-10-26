/*
 * Provides IO functions to interface between the BIO buffers and TCL
 * applications when using stacked channels.
 *
 * Copyright (C) 1997-2000 Matt Newman <matt@novadigm.com>
 * Copyright (C) 2000 Ajuba Solutions
 * Copyright (C) 2024 Brian O'Hagan
 *
 * Additional credit is due for Andreas Kupries (a.kupries@westend.com), for
 * providing the Tcl_ReplaceChannel mechanism and working closely with me
 * to enhance it to support full fileevent semantics.
 *
 * Also work done by the follow people provided the impetus to do this "right":
 *    tclSSL (Colin McCormack, Shared Technology)
 *    SSLtcl (Peter Antman)
 *
 */

/*
		tlsBIO.c				tlsIO.c
  +------+                         +-----+                                     +------+
  |      |Tcl_WriteRaw <-- BioWrite| SSL |BIO_write <-- TlsOutputProc <-- Write|      |
  |socket|      <encrypted>        | BIO |            <unencrypted>            | App  |
  |      |Tcl_ReadRaw  -->  BioRead|     |BIO_Read  --> TlsInputProc  -->  Read|      |
  +------+                         +-----+                                     +------+
*/

#include "tlsInt.h"
#include <errno.h>

/*
 *-----------------------------------------------------------------------------
 *
 * TlsBlockModeProc --
 *
 *	This procedure is invoked by the generic IO level to set channel to
 *	blocking or nonblocking mode. Called by the generic I/O layer whenever
 *	the Tcl_SetChannelOption() function is used with option -blocking.
 *
 * Results:
 *    0 if successful or POSIX error code if failed.
 *
 * Side effects:
 *    Sets the device into blocking or nonblocking mode.
 *
 *-----------------------------------------------------------------------------
 */
static int TlsBlockModeProc(ClientData instanceData, int mode) {
    State *statePtr = (State *) instanceData;

    if (mode == TCL_MODE_NONBLOCKING) {
	statePtr->flags |= TLS_TCL_ASYNC;
    } else {
	statePtr->flags &= ~(TLS_TCL_ASYNC);
    }
    return 0;
}

/*
 *-----------------------------------------------------------------------------
 *
 * TlsCloseProc --
 *
 *	This procedure is invoked by the generic IO level to perform channel
 *	type specific cleanup when a SSL socket based channel is closed.
 *	Called by the generic I/O layer whenever the Tcl_Close() function is
 *	used.
 *
 * Results:
 *    0 if successful or POSIX error code if failed.
 *
 * Side effects:
 *    Closes the socket of the channel.
 *
 *-----------------------------------------------------------------------------
 */
static int TlsCloseProc(ClientData instanceData, Tcl_Interp *interp) {
    State *statePtr = (State *) instanceData;

    dprintf("TlsCloseProc(%p)", (void *) statePtr);

    /* Flush any pending data */

    /* Send shutdown notification. Will return 0 while in process, then 1 when complete. */
    /* Closes the write direction of the connection; the read direction is closed by the peer. */
    /* Does not affect socket state. Don't call after fatal error. */
    if (statePtr->ssl != NULL && !(statePtr->flags & TLS_TCL_HANDSHAKE_FAILED)) {
	SSL_shutdown(statePtr->ssl);
    }

    /* Tls_Free calls Tls_Clean */
    Tcl_EventuallyFree((ClientData)statePtr, Tls_Free);
    return 0;
}

/*
 *-----------------------------------------------------------------------------
 *
 * TlsClose2Proc --
 *
 *	Similar to TlsCloseProc, but allows for separate close read and write
 *	side of channel.
 *
 *-----------------------------------------------------------------------------
 */
static int TlsClose2Proc(ClientData instanceData,    /* The socket state. */
    Tcl_Interp *interp,		/* For errors - can be NULL. */
    int flags)			/* Flags to close read and/or write side of channel */
{
    State *statePtr = (State *) instanceData;

    dprintf("TlsClose2Proc(%p)", (void *) statePtr);

    if ((flags & (TCL_CLOSE_READ|TCL_CLOSE_WRITE)) == 0) {
	return TlsCloseProc(instanceData, interp);
    }
    return EINVAL;
}

/*
 *-----------------------------------------------------------------------------
 *
 * Tls_WaitForConnect --
 *
 *	Perform connect (client) or accept (server) function. Also performs
 *	equivalent of handshake function.
 *
 * Result:
 *    0 if successful, -1 if failed.
 *
 * Side effects:
 *    Issues SSL_accept or SSL_connect
 *
 *-----------------------------------------------------------------------------
 */
int Tls_WaitForConnect(State *statePtr, int *errorCodePtr, int handshakeFailureIsPermanent) {
    unsigned long backingError;
    int err, rc = 0;
    int bioShouldRetry;
    *errorCodePtr = 0;

    dprintf("WaitForConnect(%p)", (void *) statePtr);
    dprintFlags(statePtr);

    /* Can also check SSL_is_init_finished(ssl) */
    if (!(statePtr->flags & TLS_TCL_INIT)) {
	dprintf("Tls_WaitForConnect called on already initialized channel -- returning with immediate success");
	return 0;
    }

    if (statePtr->flags & TLS_TCL_HANDSHAKE_FAILED) {
	/*
	 * Different types of operations have different requirements
	 * SSL being established
	 */
	if (handshakeFailureIsPermanent) {
	    dprintf("Asked to wait for a TLS handshake that has already failed.  Returning fatal error");
	    *errorCodePtr = ECONNABORTED;
	} else {
	    dprintf("Asked to wait for a TLS handshake that has already failed.  Returning soft error");
	    *errorCodePtr = ECONNRESET;
	}
	Tls_Error(statePtr, "Wait for failed handshake");
	return -1;
    }

    for (;;) {
	ERR_clear_error();
	BIO_clear_retry_flags(statePtr->bio);

	/* Not initialized yet! Also calls SSL_do_handshake(). */
	if (statePtr->flags & TLS_TCL_SERVER) {
	    dprintf("Calling SSL_accept()");
	    err = SSL_accept(statePtr->ssl);

	} else {
	    dprintf("Calling SSL_connect()");
	    err = SSL_connect(statePtr->ssl);
	}

	/* 1=successful, 0=not successful and shut down, <0=fatal error */
	if (err > 0) {
	    dprintf("Accept or connect was successful");

	    err = BIO_flush(statePtr->bio);
	    if (err <= 0) {
		dprintf("Flushing the lower layers failed, this will probably terminate this session");
	    }
	} else {
	    dprintf("Accept or connect failed");
	}

	/* Same as SSL_want, but also checks the error queue */
	rc = SSL_get_error(statePtr->ssl, err);
	backingError = ERR_get_error();
	if (rc != SSL_ERROR_NONE) {
	    dprintf("Got error: %i (rc = %i)", err, rc);
	    dprintf("Got error: %s", ERR_reason_error_string(backingError));
	}

	/* The retry flag is set by the BIO_set_retry_* functions */
	bioShouldRetry = BIO_should_retry(statePtr->bio);

	if (err <= 0) {
	    if (rc == SSL_ERROR_WANT_CONNECT || rc == SSL_ERROR_WANT_ACCEPT) {
		bioShouldRetry = 1;
	    } else if (rc == SSL_ERROR_WANT_READ) {
		bioShouldRetry = 1;
		statePtr->want |= TCL_READABLE;
	    } else if (rc == SSL_ERROR_WANT_WRITE) {
		bioShouldRetry = 1;
		statePtr->want |= TCL_WRITABLE;
	    } else if (BIO_should_retry(statePtr->bio)) {
		bioShouldRetry = 1;
	    } else if (rc == SSL_ERROR_SYSCALL && Tcl_GetErrno() == EAGAIN) {
		bioShouldRetry = 1;
	    }
	}

	if (bioShouldRetry) {
	    dprintf("The I/O did not complete -- but we should try it again");

	    if (statePtr->flags & TLS_TCL_ASYNC) {
		dprintf("Returning EAGAIN so that it can be retried later");
		*errorCodePtr = EAGAIN;
		Tls_Error(statePtr, "Handshake not complete, will retry later");
		return -1;
	    } else {
		dprintf("Doing so now");
		continue;
	    }
	}

	dprintf("We have either completely established the session or completely failed it -- there is no more need to ever retry it though");
	break;
    }

    switch (rc) {
	case SSL_ERROR_NONE:
	    /* The TLS/SSL I/O operation completed successfully */
	    dprintf("The connection is good");
	    *errorCodePtr = 0;
	    break;

	case SSL_ERROR_SSL:
	    /* A non-recoverable, fatal error in the SSL library occurred, usually a protocol error */
	    dprintf("SSL_ERROR_SSL: Got permanent fatal SSL error, aborting immediately");
	    if (SSL_get_verify_result(statePtr->ssl) != X509_V_OK) {
		Tls_Error(statePtr, X509_verify_cert_error_string(SSL_get_verify_result(statePtr->ssl)));
	    }
	    if (backingError != 0) {
		Tls_Error(statePtr, ERR_reason_error_string(backingError));
	    }
	    statePtr->flags |= TLS_TCL_HANDSHAKE_FAILED;
	    *errorCodePtr = ECONNABORTED;
	    return -1;

	case SSL_ERROR_SYSCALL:
	    /* Some non-recoverable, fatal I/O error occurred */
	    dprintf("SSL_ERROR_SYSCALL");

	    if (backingError == 0 && err == 0) {
		dprintf("EOF reached")
		*errorCodePtr = ECONNRESET;
		Tls_Error(statePtr, "(unexpected) EOF reached");

	    } else if (backingError == 0 && err == -1) {
		dprintf("I/O error occurred (errno = %lu)", (unsigned long) Tcl_GetErrno());
		*errorCodePtr = Tcl_GetErrno();
		if (*errorCodePtr == ECONNRESET) {
		    *errorCodePtr = ECONNABORTED;
		}
		Tls_Error(statePtr, Tcl_ErrnoMsg(*errorCodePtr));

	    } else {
		dprintf("I/O error occurred (backingError = %lu)", backingError);
		*errorCodePtr = Tcl_GetErrno();
		if (*errorCodePtr == ECONNRESET) {
		    *errorCodePtr = ECONNABORTED;
		}
		Tls_Error(statePtr, ERR_reason_error_string(backingError));
	    }

	    statePtr->flags |= TLS_TCL_HANDSHAKE_FAILED;
	    return -1;

	case SSL_ERROR_ZERO_RETURN:
	    /* Peer has closed the connection by sending the close_notify alert. Can't read, but can write. */
	    /* Need to return an EOF, so channel is closed which will send an SSL_shutdown(). */
	    dprintf("SSL_ERROR_ZERO_RETURN: Connect returned an invalid value...");
	    *errorCodePtr = ECONNRESET;
	    Tls_Error(statePtr, "Peer has closed the connection for writing by sending the close_notify alert");
	    return -1;

	case SSL_ERROR_WANT_READ:
	    /* More data must be read from the underlying BIO layer in order to complete the actual SSL_*() operation.  */
	    dprintf("SSL_ERROR_WANT_READ");
	    BIO_set_retry_read(statePtr->bio);
	    *errorCodePtr = EAGAIN;
	    dprintf("ERR(%d, %d) ", rc, *errorCodePtr);
	    statePtr->want |= TCL_READABLE;
	    return -1;

	case SSL_ERROR_WANT_WRITE:
	    /* There is data in the SSL buffer that must be written to the underlying BIO in order to complete the SSL_*() operation. */
	    dprintf("SSL_ERROR_WANT_WRITE");
	    BIO_set_retry_write(statePtr->bio);
	    *errorCodePtr = EAGAIN;
	    dprintf("ERR(%d, %d) ", rc, *errorCodePtr);
	    statePtr->want |= TCL_WRITABLE;
	    return -1;

	case SSL_ERROR_WANT_CONNECT:
	    /* Connect would have blocked. */
	    dprintf("SSL_ERROR_WANT_CONNECT");
	    BIO_set_retry_special(statePtr->bio);
	    BIO_set_retry_reason(statePtr->bio, BIO_RR_CONNECT);
	    *errorCodePtr = EAGAIN;
	    dprintf("ERR(%d, %d) ", rc, *errorCodePtr);
	    return -1;

	case SSL_ERROR_WANT_ACCEPT:
	    /* Accept would have blocked */
	    dprintf("SSL_ERROR_WANT_ACCEPT");
	    BIO_set_retry_special(statePtr->bio);
	    BIO_set_retry_reason(statePtr->bio, BIO_RR_ACCEPT);
	    *errorCodePtr = EAGAIN;
	    dprintf("ERR(%d, %d) ", rc, *errorCodePtr);
	    return -1;

	case SSL_ERROR_WANT_X509_LOOKUP:
	    /* App callback set by SSL_CTX_set_client_cert_cb has asked to be called again */
	    /* The operation did not complete because an application callback set by SSL_CTX_set_client_cert_cb() has asked to be called again. */
	    dprintf("SSL_ERROR_WANT_X509_LOOKUP");
	    BIO_set_retry_special(statePtr->bio);
	    BIO_set_retry_reason(statePtr->bio, BIO_RR_SSL_X509_LOOKUP);
	    *errorCodePtr = EAGAIN;
	    dprintf("ERR(%d, %d) ", rc, *errorCodePtr);
	    return -1;

	case SSL_ERROR_WANT_ASYNC:
	    /* Used with flag SSL_MODE_ASYNC, op didn't complete because an async engine is still processing data */
	case SSL_ERROR_WANT_ASYNC_JOB:
	    /* The asynchronous job could not be started because there were no async jobs available in the pool. */
	case SSL_ERROR_WANT_CLIENT_HELLO_CB:
	    /* The operation did not complete because an application callback set by SSL_CTX_set_client_hello_cb() has asked to be called again. */
#if OPENSSL_VERSION_NUMBER >= 0x30000000L
	case SSL_ERROR_WANT_RETRY_VERIFY:
	    /* The operation did not complete because a certificate verification callback has asked to be called again via SSL_set_retry_verify(3). */
#endif
	default:
	    /* The operation did not complete and should be retried later. */
	    dprintf("Operation did not complete, call function again later: %i", rc);
	    *errorCodePtr = EAGAIN;
	    dprintf("ERR(%d, %d) ", rc, *errorCodePtr);
	    Tls_Error(statePtr, "Operation did not complete, call function again later");
	    return -1;
    }

    dprintf("Removing the \"TLS_TCL_INIT\" flag since we have completed the handshake");
    statePtr->flags &= ~TLS_TCL_INIT;

    dprintf("Returning in success");
    *errorCodePtr = 0;
    return 0;
}

/*
 *-----------------------------------------------------------------------------
 *
 * TlsInputProc --
 *
 *	This procedure is invoked by the generic I/O layer to read data from
 *	the BIO whenever the Tcl_Read(), Tcl_ReadChars, Tcl_Gets, and
 *	Tcl_GetsObj functions are used. Equivalent to SSL_read_ex and SSL_read.
 *
 * Results:
 *	Returns the number of bytes read or -1 on error. Sets errorCodePtr to
 *	a POSIX error code if an error occurred, or 0 if none.
 *
 * Side effects:
 *    Reads input from the input device of the channel.
 *
 * Data is received in whole blocks known as records from the peer. A whole
 * record is processed (e.g. decrypted) in one go and is buffered by OpenSSL
 * until it is read by the application via a call to SSL_read.
 *
 *-----------------------------------------------------------------------------
 */
static int TlsInputProc(ClientData instanceData, char *buf, int bufSize, int *errorCodePtr) {
    unsigned long backingError;
    State *statePtr = (State *) instanceData;
    int bytesRead, err;
    *errorCodePtr = 0;

    dprintf("Read(%d)", bufSize);

    /* Skip if user verify callback is still running */
    if (statePtr->flags & TLS_TCL_CALLBACK) {
	dprintf("Callback is running, reading 0 bytes");
	return 0;
    }

    /* If not initialized, do connect */
    /* Can also check SSL_is_init_finished(ssl) */
    if (statePtr->flags & TLS_TCL_INIT) {
	int tlsConnect;

	dprintf("Calling Tls_WaitForConnect");

	tlsConnect = Tls_WaitForConnect(statePtr, errorCodePtr, 0);
	if (tlsConnect < 0) {
	    dprintf("Got an error waiting to connect (tlsConnect = %i, *errorCodePtr = %i)", tlsConnect, *errorCodePtr);
	    Tls_Error(statePtr, strerror(*errorCodePtr));

	    bytesRead = -1;
	    if (*errorCodePtr == ECONNRESET) {
		dprintf("Got connection reset");
		/* Soft EOF */
		*errorCodePtr = 0;
		bytesRead = 0;
	    }
	    return bytesRead;
	}
    }

    /*
     * We need to clear the SSL error stack now because we sometimes reach
     * this function with leftover errors in the stack.  If BIO_read
     * returns -1 and intends EAGAIN, there is a leftover error, it will be
     * misconstrued as an error, not EAGAIN.
     *
     * Alternatively, we may want to handle the <0 return codes from
     * BIO_read specially (as advised in the RSA docs).  TLS's lower level BIO
     * functions play with the retry flags though, and this seems to work
     * correctly.  Similar fix in TlsOutputProc. - hobbs
     */
    ERR_clear_error();
    BIO_clear_retry_flags(statePtr->bio);
    bytesRead = BIO_read(statePtr->bio, buf, bufSize);
    dprintf("BIO_read -> %d", bytesRead);

    /* Same as SSL_want, but also checks the error queue */
    err = SSL_get_error(statePtr->ssl, bytesRead);
    backingError = ERR_get_error();

    if (bytesRead <= 0) {
	/* The retry flag is set by the BIO_set_retry_* functions */
	if (BIO_should_retry(statePtr->bio)) {
	    dprintf("Read failed with code=%d, bytes read=%d: should retry", err, bytesRead);
	    /* Some docs imply we should redo the BIO_read now */
	} else {
	    dprintf("Read failed with code=%d, bytes read=%d: error condition", err, bytesRead);
	}

	dprintf("BIO is EOF %d", BIO_eof(statePtr->bio));

	/* These are the same as BIO_retry_type */
	if (BIO_should_read(statePtr->bio)) {
	    dprintf("BIO has insufficient data to read and return");
	    statePtr->want |= TCL_READABLE;
	}
	if (BIO_should_write(statePtr->bio)) {
	    dprintf("BIO has pending data to write");
	    statePtr->want |= TCL_WRITABLE;
	}
	if (BIO_should_io_special(statePtr->bio)) {
	    int reason = BIO_get_retry_reason(statePtr->bio);
	    dprintf("BIO has some special condition other than read or write: code=%d", reason);
	}
	dprintf("BIO has pending data to write");
    }

    switch (err) {
	case SSL_ERROR_NONE:
	    /* I/O operation completed */
	    dprintf("SSL_ERROR_NONE");
	    dprintBuffer(buf, bytesRead);
	    break;

	case SSL_ERROR_SSL:
	    /* A non-recoverable, fatal error in the SSL library occurred, usually a protocol error */
	    dprintf("SSL error, indicating that the connection has been aborted");
	    if (backingError != 0) {
		Tls_Error(statePtr, ERR_reason_error_string(backingError));
	    } else if (SSL_get_verify_result(statePtr->ssl) != X509_V_OK) {
		Tls_Error(statePtr, X509_verify_cert_error_string(SSL_get_verify_result(statePtr->ssl)));
	    } else {
		Tls_Error(statePtr, "Unknown SSL error");
	    }
	    *errorCodePtr = ECONNABORTED;
	    bytesRead = -1;

#if OPENSSL_VERSION_NUMBER >= 0x30000000L
	    /* Unexpected EOF from the peer for OpenSSL 3.0+ */
	    if (ERR_GET_REASON(backingError) == SSL_R_UNEXPECTED_EOF_WHILE_READING) {
		dprintf("(Unexpected) EOF reached")
		*errorCodePtr = 0;
		bytesRead = 0;
		Tls_Error(statePtr, "EOF reached");
	    }
#endif
	    break;

	case SSL_ERROR_WANT_READ:
	    /* Op did not complete due to not enough data was available. Retry later. */
	    dprintf("Got SSL_ERROR_WANT_READ, mapping this to EAGAIN");
	    *errorCodePtr = EAGAIN;
	    bytesRead = -1;
	    statePtr->want |= TCL_READABLE;
	    Tls_Error(statePtr, "SSL_ERROR_WANT_READ");
	    BIO_set_retry_read(statePtr->bio);
	    break;

	case SSL_ERROR_WANT_WRITE:
	    /* Op did not complete due to unable to sent all data to the BIO. Retry later. */
	    dprintf("Got SSL_ERROR_WANT_WRITE, mapping this to EAGAIN");
	    *errorCodePtr = EAGAIN;
	    bytesRead = -1;
	    statePtr->want |= TCL_WRITABLE;
	    Tls_Error(statePtr, "SSL_ERROR_WANT_WRITE");
	    BIO_set_retry_write(statePtr->bio);
	    break;

	case SSL_ERROR_WANT_X509_LOOKUP:
	    /* Op didn't complete since callback set by SSL_CTX_set_client_cert_cb() asked to be called again */
	    dprintf("Got SSL_ERROR_WANT_X509_LOOKUP, mapping it to EAGAIN");
	    *errorCodePtr = EAGAIN;
	    bytesRead = -1;
	    Tls_Error(statePtr, "SSL_ERROR_WANT_X509_LOOKUP");
	    break;

	case SSL_ERROR_SYSCALL:
	    /* Some non-recoverable, fatal I/O error occurred */
	    dprintf("SSL_ERROR_SYSCALL");

	    if (backingError == 0 && bytesRead == 0) {
		/* Unexpected EOF from the peer for OpenSSL 1.1 */
		dprintf("(Unexpected) EOF reached")
		*errorCodePtr = 0;
		bytesRead = 0;
		Tls_Error(statePtr, "EOF reached");

	    } else if (backingError == 0 && bytesRead == -1) {
		dprintf("I/O error occurred (errno = %lu)", (unsigned long) Tcl_GetErrno());
		*errorCodePtr = Tcl_GetErrno();
		bytesRead = -1;
		Tls_Error(statePtr, Tcl_ErrnoMsg(*errorCodePtr));

	    } else {
		dprintf("I/O error occurred (backingError = %lu)", backingError);
		*errorCodePtr = Tcl_GetErrno();
		bytesRead = -1;
		Tls_Error(statePtr, ERR_reason_error_string(backingError));
	    }
	    break;

	case SSL_ERROR_ZERO_RETURN:
	    /* Peer has closed the connection by sending the close_notify alert. Can't read, but can write. */
	    /* Need to return an EOF, so channel is closed which will send an SSL_shutdown(). */
	    dprintf("Got SSL_ERROR_ZERO_RETURN, this means an EOF has been reached");
	    bytesRead = 0;
	    *errorCodePtr = 0;
	    Tls_Error(statePtr, "Peer has closed the connection for writing by sending the close_notify alert");
	    break;

	case SSL_ERROR_WANT_ASYNC:
	    /* Used with flag SSL_MODE_ASYNC, op didn't complete because an async engine is still processing data */
	    dprintf("Got SSL_ERROR_WANT_ASYNC, mapping this to EAGAIN");
	    bytesRead = -1;
	    *errorCodePtr = EAGAIN;
	    Tls_Error(statePtr, "SSL_ERROR_WANT_ASYNC");
	    break;

	default:
	    dprintf("Unknown error (err = %i), mapping to EOF", err);
	    *errorCodePtr = 0;
	    bytesRead = 0;
	    Tls_Error(statePtr, "Unknown error");
	    break;
    }

    dprintf("Input(%d) -> %d [%d]", bufSize, bytesRead, *errorCodePtr);
    return bytesRead;
}

/*
 *-----------------------------------------------------------------------------
 *
 * TlsOutputProc --
 *
 *	This procedure is invoked by the generic I/O layer to write data to the
 *	BIO whenever the the Tcl_Write(), Tcl_WriteChars, and Tcl_WriteObj
 *	functions are used. Equivalent to SSL_write_ex and SSL_write.
 *
 * Results:
 *    Returns the number of bytes written or -1 on error. Sets errorCodePtr
 *    to a POSIX error code if an error occurred, or 0 if none.
 *
 * Side effects:
 *    Writes output on the output device of the channel.
 *
 *-----------------------------------------------------------------------------
 */
static int TlsOutputProc(ClientData instanceData, const char *buf, int toWrite, int *errorCodePtr) {
    unsigned long backingError;
    State *statePtr = (State *) instanceData;
    int written, err;
    *errorCodePtr = 0;

    dprintf("Write(%p, %d)", (void *) statePtr, toWrite);
    dprintBuffer(buf, toWrite);

    /* Skip if user verify callback is still running */
    if (statePtr->flags & TLS_TCL_CALLBACK) {
	dprintf("Don't process output while callbacks are running");
	written = -1;
	*errorCodePtr = EAGAIN;
	return -1;
    }

    /* If not initialized, do connect */
    /* Can also check SSL_is_init_finished(ssl) */
    if (statePtr->flags & TLS_TCL_INIT) {
	int tlsConnect;

	dprintf("Calling Tls_WaitForConnect");

	tlsConnect = Tls_WaitForConnect(statePtr, errorCodePtr, 1);
	if (tlsConnect < 0) {
	    dprintf("Got an error waiting to connect (tlsConnect = %i, *errorCodePtr = %i)", tlsConnect, *errorCodePtr);
	    Tls_Error(statePtr, strerror(*errorCodePtr));

	    written = -1;
	    if (*errorCodePtr == ECONNRESET) {
		dprintf("Got connection reset");
		/* Soft EOF */
		*errorCodePtr = 0;
		written = 0;
	    }
	    return written;
	}
    }

    if (toWrite == 0) {
	dprintf("zero-write");
	err = BIO_flush(statePtr->bio);

	if (err <= 0) {
	    dprintf("Flushing failed");
	    Tls_Error(statePtr, "Flush failed");

	    *errorCodePtr = EIO;
	    written = 0;
	    return -1;
	}

	written = 0;
	*errorCodePtr = 0;
	return 0;
    }

    /*
     * We need to clear the SSL error stack now because we sometimes reach
     * this function with leftover errors in the stack.  If BIO_write
     * returns -1 and intends EAGAIN, there is a leftover error, it will be
     * misconstrued as an error, not EAGAIN.
     *
     * Alternatively, we may want to handle the <0 return codes from
     * BIO_write specially (as advised in the RSA docs).  TLS's lower level
     * BIO functions play with the retry flags though, and this seems to
     * work correctly.  Similar fix in TlsInputProc. - hobbs
     */
    ERR_clear_error();
    BIO_clear_retry_flags(statePtr->bio);
    written = BIO_write(statePtr->bio, buf, toWrite);
    dprintf("BIO_write(%p, %d) -> [%d]", (void *) statePtr, toWrite, written);

    /* Same as SSL_want, but also checks the error queue */
    err = SSL_get_error(statePtr->ssl, written);
    backingError = ERR_get_error();

    if (written <= 0) {
	/* The retry flag is set by the BIO_set_retry_* functions */
	if (BIO_should_retry(statePtr->bio)) {
	    dprintf("Write failed with code %d, bytes written=%d: should retry", err, written);
	} else {
	    dprintf("Write failed with code %d, bytes written=%d: error condition", err, written);
	}

	/* These are the same as BIO_retry_type */
	if (BIO_should_read(statePtr->bio)) {
	    dprintf("BIO has insufficient data to read and return");
	}
	if (BIO_should_write(statePtr->bio)) {
	    dprintf("BIO has pending data to write");
	}
	if (BIO_should_io_special(statePtr->bio)) {
	    int reason = BIO_get_retry_reason(statePtr->bio);
	    dprintf("BIO has some special condition other than read or write: code=%d", reason);
	}
	dprintf("BIO has pending data to write");

    } else {
	BIO_flush(statePtr->bio);
    }

    switch (err) {
	case SSL_ERROR_NONE:
	    /* I/O operation completed */
	    dprintf("SSL_ERROR_NONE");
	    if (written < 0) {
		written = 0;
	    }
	    break;

	case SSL_ERROR_SSL:
	    /* A non-recoverable, fatal error in the SSL library occurred, usually a protocol error */
	    dprintf("SSL error, indicating that the connection has been aborted");
	    if (backingError != 0) {
		Tls_Error(statePtr, ERR_reason_error_string(backingError));
	    } else if (SSL_get_verify_result(statePtr->ssl) != X509_V_OK) {
		Tls_Error(statePtr, X509_verify_cert_error_string(SSL_get_verify_result(statePtr->ssl)));
	    } else {
		Tls_Error(statePtr, "Unknown SSL error");
	    }
	    *errorCodePtr = ECONNABORTED;
	    written = -1;
	    break;

	case SSL_ERROR_WANT_READ:
	    /* Op did not complete due to not enough data was available. Retry later. */
	    dprintf("Got SSL_ERROR_WANT_READ, mapping it to EAGAIN");
	    *errorCodePtr = EAGAIN;
	    written = -1;
	    statePtr->want |= TCL_READABLE;
	    Tls_Error(statePtr, "SSL_ERROR_WANT_READ");
	    BIO_set_retry_read(statePtr->bio);
	    break;

	case SSL_ERROR_WANT_WRITE:
	    /* Op did not complete due to unable to sent all data to the BIO. Retry later. */
	    dprintf("Got SSL_ERROR_WANT_WRITE, mapping it to EAGAIN");
	    *errorCodePtr = EAGAIN;
	    written = -1;
	    statePtr->want |= TCL_WRITABLE;
	    Tls_Error(statePtr, "SSL_ERROR_WANT_WRITE");
	    BIO_set_retry_write(statePtr->bio);
	    break;

	case SSL_ERROR_WANT_X509_LOOKUP:
	    /* Op didn't complete since callback set by SSL_CTX_set_client_cert_cb() asked to be called again */
	    dprintf("Got SSL_ERROR_WANT_X509_LOOKUP, mapping it to EAGAIN");
	    *errorCodePtr = EAGAIN;
	    written = -1;
	    Tls_Error(statePtr, "SSL_ERROR_WANT_X509_LOOKUP");
	    break;

	case SSL_ERROR_SYSCALL:
	    /* Some non-recoverable, fatal I/O error occurred */
	    dprintf("SSL_ERROR_SYSCALL");

	    if (backingError == 0 && written == 0) {
		dprintf("EOF reached")
		*errorCodePtr = 0;
		written = 0;
		Tls_Error(statePtr, "EOF reached");

	    } else if (backingError == 0 && written == -1) {
		dprintf("I/O error occurred (errno = %lu)", (unsigned long) Tcl_GetErrno());
		*errorCodePtr = Tcl_GetErrno();
		written = -1;
		Tls_Error(statePtr, Tcl_ErrnoMsg(*errorCodePtr));

	    } else {
		dprintf("I/O error occurred (backingError = %lu)", backingError);
		*errorCodePtr = Tcl_GetErrno();
		written = -1;
		Tls_Error(statePtr, ERR_reason_error_string(backingError));
	    }
	    break;

	case SSL_ERROR_ZERO_RETURN:
	    /* Peer has closed the connection by sending the close_notify alert. Can't read, but can write. */
	    /* Need to return an EOF, so channel is closed which will send an SSL_shutdown(). */
	    dprintf("Got SSL_ERROR_ZERO_RETURN, this means an EOF has been reached");
	    written = 0;
	    *errorCodePtr = 0;
	    Tls_Error(statePtr, "Peer has closed the connection for writing by sending the close_notify alert");
	    break;

	case SSL_ERROR_WANT_ASYNC:
	    /* Used with flag SSL_MODE_ASYNC, op didn't complete because an async engine is still processing data */
	    dprintf("Got SSL_ERROR_WANT_ASYNC, mapping this to EAGAIN");
	    *errorCodePtr = EAGAIN;
	    written = -1;
	    Tls_Error(statePtr, "SSL_ERROR_WANT_ASYNC");
	    break;

	default:
	    dprintf("unknown error: %d", err);
	    Tls_Error(statePtr, "Unknown error");
	    break;
    }

    dprintf("Output(%d) -> %d", toWrite, written);
    return written;
}

/*
 *-----------------------------------------------------------------------------
 *
 * Tls_GetParent --
 *
 *    Get parent channel for a stacked channel.
 *
 * Results:
 *    Tcl_Channel or NULL if none.
 *
 *-----------------------------------------------------------------------------
 */
Tcl_Channel Tls_GetParent(State *statePtr, int maskFlags) {
    dprintf("Requested to get parent of channel %p", statePtr->self);

    if ((statePtr->flags & ~maskFlags) & TLS_TCL_FASTPATH) {
	dprintf("Asked to get the parent channel while we are using FastPath -- returning NULL");
	return NULL;
    }
    return Tcl_GetStackedChannel(statePtr->self);
}

/*
 *-----------------------------------------------------------------------------
 *
 * TlsSetOptionProc --
 *
 *	Sets an option to value for a SSL socket based channel. Called by the
 *	generic I/O layer whenever the Tcl_SetChannelOption() function is used.
 *
 * Results:
 *    TCL_OK if successful or TCL_ERROR if failed.
 *
 * Side effects:
 *    Updates channel option to new value.
 *
 *-----------------------------------------------------------------------------
 */
static int
TlsSetOptionProc(ClientData instanceData,    /* Socket state. */
    Tcl_Interp *interp,		/* For errors - can be NULL. */
    const char *optionName,	/* Name of the option to set the value for, or
				 * NULL to get all options and their values. */
    const char *optionValue)	/* Value for option. */
{
    State *statePtr = (State *) instanceData;
    Tcl_Channel parent = Tls_GetParent(statePtr, TLS_TCL_FASTPATH);
    Tcl_DriverSetOptionProc *setOptionProc;

    dprintf("Called");

    /* Pass to parent */
    setOptionProc = Tcl_ChannelSetOptionProc(Tcl_GetChannelType(parent));
    if (setOptionProc != NULL) {
	return (*setOptionProc)(Tcl_GetChannelInstanceData(parent), interp, optionName, optionValue);
    }
    /*
     * Request for a specific option has to fail, we don't have any.
     */
    return Tcl_BadChannelOption(interp, optionName, "");
}

/*
 *-------------------------------------------------------------------
 *
 * TlsGetOptionProc --
 *
 *	Get a option's value for a SSL socket based channel, or a list of all
 *	options and their values. Called by the generic I/O layer whenever the
 *	Tcl_GetChannelOption() function is used.
 *
 *
 * Results:
 *	A standard Tcl result. The value of the specified option or a list of
 *	all options and their values is returned in the supplied DString.
 *
 * Side effects:
 *    None.
 *
 *-------------------------------------------------------------------
 */
static int
TlsGetOptionProc(ClientData instanceData,    /* Socket state. */
    Tcl_Interp *interp,		/* For errors - can be NULL. */
    const char *optionName,	/* Name of the option to retrieve the value for, or
				 * NULL to get all options and their values. */
    Tcl_DString *optionValue)	/* Where to store the computed value initialized by caller. */
{
    State *statePtr = (State *) instanceData;
    Tcl_Channel parent = Tls_GetParent(statePtr, TLS_TCL_FASTPATH);
    Tcl_DriverGetOptionProc *getOptionProc;

    dprintf("Called");

    /* Pass to parent */
    getOptionProc = Tcl_ChannelGetOptionProc(Tcl_GetChannelType(parent));
    if (getOptionProc != NULL) {
	return (*getOptionProc)(Tcl_GetChannelInstanceData(parent), interp, optionName, optionValue);
    } else if (optionName == (char*) NULL) {
	/*
	 * Request is query for all options, this is ok.
	 */
	return TCL_OK;
    }
    /*
     * Request for a specific option has to fail, we don't have any.
     */
    return Tcl_BadChannelOption(interp, optionName, "");
}

/*
 *-----------------------------------------------------------------------------
 *
 *    TlsChannelHandlerTimer --
 *
 *	Called by the notifier via a timer, to flush out data waiting in
 *	channel buffers. called by the generic I/O layer whenever the
 *	Tcl_GetChannelHandle() function is used.
 *
 * Results:
 *        None.
 *
 * Side effects:
 *	Creates notification event.
 *
 *-----------------------------------------------------------------------------
 */
static void TlsChannelHandlerTimer(ClientData clientData) {
    State *statePtr = (State *) clientData;
    int mask = statePtr->want; /* Init to SSL_ERROR_WANT_READ and SSL_ERROR_WANT_WRITE */

    dprintf("Called");

    statePtr->timer = (Tcl_TimerToken) NULL;

    /* Check for amount of data pending in BIO write buffer */
    if (BIO_wpending(statePtr->bio)) {
	dprintf("[chan=%p] BIO writable", statePtr->self);

	mask |= TCL_WRITABLE;
    }

    /* Check for amount of data pending in BIO read buffer */
    if (BIO_pending(statePtr->bio)) {
	dprintf("[chan=%p] BIO readable", statePtr->self);

	mask |= TCL_READABLE;
    }

    /* Notify the generic IO layer that the mask events have occurred on the channel */
    dprintf("Notifying ourselves");
    Tcl_NotifyChannel(statePtr->self, mask);
    statePtr->want = 0;

    dprintf("Returning");

    return;
}

/*
 *-----------------------------------------------------------------------------
 *
 * TlsWatchProc --
 *
 *	Set up the event notifier to watch for events of interest from this
 *	channel. Called by the generic I/O layer whenever the user (or the
 *	system) announces its (dis)interest in events on the channel. This is
 *	called repeatedly.
 *
 * Results:
 *    None.
 *
 * Side effects:
 *	Sets up the time-based notifier so that future events on the channel
 *	will be seen by TCL.
 *
 *-----------------------------------------------------------------------------
 */
static void
TlsWatchProc(ClientData instanceData,    /* The socket state. */
    int mask)			/* Events of interest; an OR-ed combination of
				 * TCL_READABLE, TCL_WRITABLE and TCL_EXCEPTION. */
{
    Tcl_Channel     parent;
    State *statePtr = (State *) instanceData;
    Tcl_DriverWatchProc *watchProc;
    int pending = 0;

    dprintf("TlsWatchProc(0x%x)", mask);
    dprintFlags(statePtr);

    /* Pretend to be dead as long as the verify callback is running.
     * Otherwise that callback could be invoked recursively. */
    if (statePtr->flags & TLS_TCL_CALLBACK) {
	dprintf("Callback is on-going, doing nothing");
	return;
    }

    parent = Tls_GetParent(statePtr, TLS_TCL_FASTPATH);

    if (statePtr->flags & TLS_TCL_HANDSHAKE_FAILED) {
	dprintf("Asked to watch a socket with a failed handshake -- nothing can happen here");
	dprintf("Unregistering interest in the lower channel");

	watchProc = Tcl_ChannelWatchProc(Tcl_GetChannelType(parent));
	watchProc(Tcl_GetChannelInstanceData(parent), 0);
	statePtr->watchMask = 0;
	return;
    }

    statePtr->watchMask = mask;

    /* No channel handlers any more. We will be notified automatically
     * about events on the channel below via a call to our
     * 'TransformNotifyProc'. But we have to pass the interest down now.
     * We are allowed to add additional 'interest' to the mask if we want
     * to. But this transformation has no such interest. It just passes
     * the request down, unchanged.
     */
    dprintf("Registering our interest in the lower channel (chan=%p)", (void *) parent);
    watchProc = Tcl_ChannelWatchProc(Tcl_GetChannelType(parent));
    watchProc(Tcl_GetChannelInstanceData(parent), mask);

    /* Do we have any pending events */
    pending = (statePtr->want || \
	((mask & TCL_READABLE) && ((Tcl_InputBuffered(statePtr->self) > 0) || (BIO_ctrl_pending(statePtr->bio) > 0))) ||
	((mask & TCL_WRITABLE) && ((Tcl_OutputBuffered(statePtr->self) > 0) || (BIO_ctrl_wpending(statePtr->bio) > 0))));

    if (!(mask & TCL_READABLE) || pending == 0) {
	/* Remove timer, if any */
	if (statePtr->timer != (Tcl_TimerToken) NULL) {
	    dprintf("A timer was found, deleting it");
	    Tcl_DeleteTimerHandler(statePtr->timer);
	    statePtr->timer = (Tcl_TimerToken) NULL;
	}

    } else {
	/* Add timer, if none */
	if (statePtr->timer == (Tcl_TimerToken) NULL) {
	    dprintf("Creating a new timer since data appears to be waiting");
	    statePtr->timer = Tcl_CreateTimerHandler(TLS_TCL_DELAY, TlsChannelHandlerTimer, (ClientData) statePtr);
	}
    }
}

/*
 *-----------------------------------------------------------------------------
 *
 * TlsGetHandleProc --
 *
 *	This procedure is invoked by the generic IO level to retrieve an OS
 *	specific handle associated with the channel. Not used for transforms.
 *
 * Results:
 *    The appropriate Tcl_File handle or NULL if none.
 *
 * Side effects:
 *    None.
 *
 *-----------------------------------------------------------------------------
 */
static int TlsGetHandleProc(ClientData instanceData,    /* Socket state. */
    int direction,		/* TCL_READABLE or TCL_WRITABLE */
    ClientData *handlePtr)	/* Handle associated with the channel */
{
    State *statePtr = (State *) instanceData;

    return Tcl_GetChannelHandle(Tls_GetParent(statePtr, TLS_TCL_FASTPATH), direction, handlePtr);
}

/*
 *-----------------------------------------------------------------------------
 *
 * TlsNotifyProc --
 *
 *	This procedure is invoked by the generic IO level to notify the channel
 *	that an event has occurred on the underlying channel. It is used by stacked channel drivers that
 *	wish to be notified of events that occur on the underlying (stacked)
 *	channel.
 *
 * Results:
 *    Type of event or 0 if failed
 *
 * Side effects:
 *    May process the incoming event by itself.
 *
 *-----------------------------------------------------------------------------
 */
static int TlsNotifyProc(ClientData instanceData,    /* Socket state. */
    int mask)			/* type of event that occurred:
				 * OR-ed combination of TCL_READABLE or TCL_WRITABLE */
{
    State *statePtr = (State *) instanceData;
    int errorCode = 0;

    dprintf("Called");

    /*
     * Delete an existing timer. It was not fired, yet we are
     * here, so the channel below generated such an event and we
     * don't have to. The renewal of the interest after the
     * execution of channel handlers will eventually cause us to
     * recreate the timer (in WatchProc).
     */
    if (statePtr->timer != (Tcl_TimerToken) NULL) {
	Tcl_DeleteTimerHandler(statePtr->timer);
	statePtr->timer = (Tcl_TimerToken) NULL;
    }

    /* Skip if user verify callback is still running */
    if (statePtr->flags & TLS_TCL_CALLBACK) {
	dprintf("Callback is on-going, returning failed");
	return 0;
    }

    /* If not initialized, do connect */
    if (statePtr->flags & TLS_TCL_INIT) {
	dprintf("Calling Tls_WaitForConnect");
	if (Tls_WaitForConnect(statePtr, &errorCode, 1) < 0) {
	    Tls_Error(statePtr, strerror(errorCode));
	    if (errorCode == EAGAIN) {
		dprintf("Async flag could be set (didn't check) and errorCode == EAGAIN:  Returning failed");

		return 0;
	    }

	    dprintf("Tls_WaitForConnect returned an error");
	}
    }

    dprintf("Returning %i", mask);

    /*
     * An event occurred in the underlying channel.  This
     * transformation doesn't process such events thus returns the
     * incoming mask unchanged.
     */
    return mask;
}

/*
 *-----------------------------------------------------------------------------
 *
 * Tls_ChannelType --
 *
 *	Defines the correct TLS channel driver handlers for this channel type.
 *
 * Results:
 *	Tcl_ChannelType structure.
 *
 * Side effects:
 *    None.
 *
 *-----------------------------------------------------------------------------
 */
static const Tcl_ChannelType tlsChannelType = {
    "tls",			/* Type name */
    TCL_CHANNEL_VERSION_5,	/* v5 channel */
    TlsCloseProc,		/* Close proc */
    TlsInputProc,		/* Input proc */
    TlsOutputProc,		/* Output proc */
    NULL,			/* Seek proc */
    TlsSetOptionProc,		/* Set option proc */
    TlsGetOptionProc,		/* Get option proc */
    TlsWatchProc,		/* Initialize notifier */
    TlsGetHandleProc,		/* Get OS handles out of channel */
    TlsClose2Proc,		/* close2proc */
    TlsBlockModeProc,		/* Set blocking/nonblocking mode*/
    NULL,			/* Flush proc */
    TlsNotifyProc,		/* Handling of events bubbling up */
    NULL,			/* Wide seek proc */
    NULL,			/* Thread action */
    NULL			/* Truncate */
};

const Tcl_ChannelType *Tls_ChannelType(void) {
    return &tlsChannelType;
}
