/*
 *  Copyright (c) 2016, The OpenThread Authors.
 *  All rights reserved.
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted provided that the following conditions are met:
 *  1. Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *  2. Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *  3. Neither the name of the copyright holder nor the
 *     names of its contributors may be used to endorse or promote products
 *     derived from this software without specific prior written permission.
 *
 *  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 *  AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 *  IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 *  ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 *  LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 *  CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 *  SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 *  INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 *  CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 *  ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 *  POSSIBILITY OF SUCH DAMAGE.
 */

/**
 * @file
 *   This file includes definitions for using mbedTLS.
 */

#ifndef SECURE_TRANSPORT_HPP_
#define SECURE_TRANSPORT_HPP_

#include "openthread-core-config.h"

#ifdef OPENTHREAD_CONFIG_TLS_API_ENABLE
#error `OPENTHREAD_CONFIG_TLS_API_ENABLE` must not be defined directly, it is determined from `COAP_SECURE_API_ENABLE` and `BLE_TCAT_ENABLE`
#endif

#if OPENTHREAD_CONFIG_COAP_SECURE_API_ENABLE || OPENTHREAD_CONFIG_BLE_TCAT_ENABLE
#define OPENTHREAD_CONFIG_TLS_API_ENABLE 1
#else
#define OPENTHREAD_CONFIG_TLS_API_ENABLE 0
#endif

#include <mbedtls/net_sockets.h>
#include <mbedtls/ssl.h>
#if defined(MBEDTLS_SSL_SRV_C) && defined(MBEDTLS_SSL_COOKIE_C)
#include <mbedtls/ssl_cookie.h>
#endif
#include <mbedtls/version.h>

#if OPENTHREAD_CONFIG_BLE_TCAT_ENABLE
#ifndef MBEDTLS_KEY_EXCHANGE_ECDHE_ECDSA_ENABLED
#error OPENTHREAD_CONFIG_BLE_TCAT_ENABLE requires MBEDTLS_KEY_EXCHANGE_ECDHE_ECDSA_ENABLED
#endif
#endif

#if OPENTHREAD_CONFIG_TLS_API_ENABLE
#if defined(MBEDTLS_BASE64_C) && defined(MBEDTLS_SSL_KEEP_PEER_CERTIFICATE)
#include <mbedtls/base64.h>
#endif
#ifdef MBEDTLS_KEY_EXCHANGE_ECDHE_ECDSA_ENABLED
#include <mbedtls/x509.h>
#include <mbedtls/x509_crl.h>
#include <mbedtls/x509_crt.h>
#include <mbedtls/x509_csr.h>
#endif
#endif

#include <openthread/coap_secure.h>

#include "common/callback.hpp"
#include "common/locator.hpp"
#include "common/log.hpp"
#include "common/message.hpp"
#include "common/non_copyable.hpp"
#include "common/random.hpp"
#include "common/timer.hpp"
#include "crypto/sha256.hpp"
#include "meshcop/meshcop_tlvs.hpp"
#include "net/socket.hpp"
#include "net/udp6.hpp"

namespace ot {

namespace MeshCoP {

class SecureTransport;
class Dtls;
#if OPENTHREAD_CONFIG_BLE_TCAT_ENABLE
class Tls;
#endif

/**
 * Represents a secure session.
 */
class SecureSession : private NonCopyable
{
    friend class SecureTransport;
    friend class Dtls;
#if OPENTHREAD_CONFIG_BLE_TCAT_ENABLE
    friend class Tls;
#endif

public:
    typedef otCoapSecureConnectEvent ConnectEvent; ///< A connect event.

    static constexpr ConnectEvent kConnected               = OT_COAP_SECURE_CONNECTED;
    static constexpr ConnectEvent kDisconnectedPeerClosed  = OT_COAP_SECURE_DISCONNECTED_PEER_CLOSED;
    static constexpr ConnectEvent kDisconnectedLocalClosed = OT_COAP_SECURE_DISCONNECTED_LOCAL_CLOSED;
    static constexpr ConnectEvent kDisconnectedMaxAttempts = OT_COAP_SECURE_DISCONNECTED_MAX_ATTEMPTS;
    static constexpr ConnectEvent kDisconnectedError       = OT_COAP_SECURE_DISCONNECTED_ERROR;

    /**
     * Function pointer which is called reporting a session connection event (when connection established or
     * disconnected).
     */
    typedef otHandleCoapSecureClientConnect ConnectedHandler;

    /**
     * Pointer is called when data is received from the session.
     *
     * @param[in]  aContext  A pointer to application-specific context.
     * @param[in]  aBuf      A pointer to the received data buffer.
     * @param[in]  aLength   Number of bytes in the received data buffer.
     */
    typedef void (*ReceiveHandler)(void *aContext, uint8_t *aBuf, uint16_t aLength);

    /**
     * Sets the connection event callback.
     *
     * @param[in]  aConnectedHandler    A pointer to a function that is called when connected or disconnected.
     * @param[in]  aContext             A pointer to arbitrary context information.
     */
    void SetConnectedCallback(ConnectedHandler aConnectedHandler, void *aContext)
    {
        mConnectedCallback.Set(aConnectedHandler, aContext);
    }

    /**
     * Sets the receive callback.
     *
     * @param[in]  aReceiveHandler      A pointer to a function that is called to receive payload.
     * @param[in]  aContext             A pointer to arbitrary context information.
     */
    void SetReceiveCallback(ReceiveHandler aReceiveHandler, void *aContext)
    {
        mReceiveCallback.Set(aReceiveHandler, aContext);
    }

    /**
     * Establishes a secure session (as client).
     *
     * @param[in]  aSockAddr       The server address to connect to.
     *
     * @retval kErrorNone          Successfully started session establishment
     * @retval kErrorInvalidState  Transport is not ready.
     */
    Error Connect(const Ip6::SockAddr &aSockAddr);

    /**
     * Disconnects the session.
     */
    void Disconnect(void) { Disconnect(kDisconnectedLocalClosed); }

    /**
     * Sends message to the secure session.
     *
     * When successful (returning `kErrorNone`), this method takes over the ownership of @p aMessage and will free
     * it after transmission. Otherwise, the caller keeps the ownership of @p aMessage.
     *
     * @param[in]  aMessage   A message to send.
     *
     * @retval kErrorNone     Successfully sent the message.
     * @retval kErrorNoBufs   @p aMessage is too long.
     */
    Error Send(Message &aMessage);

    /**
     * Returns the session's peer address.
     *
     * @return The session's message info.
     */
    const Ip6::MessageInfo &GetMessageInfo(void) const { return mMessageInfo; }

    /**
     * Indicates whether or not the session is active (connected, connecting, or disconnecting).
     *
     * @retval TRUE  If session is active.
     * @retval FALSE If session is not active.
     */
    bool IsConnectionActive(void) const { return (mState != kStateDisconnected); }

    /**
     * Indicates whether or not the session is connected.
     *
     * @retval TRUE   The session is connected.
     * @retval FALSE  The session is not connected.
     */
    bool IsConnected(void) const { return (mState == kStateConnected); }

private:
    static constexpr uint32_t kGuardTimeNewConnectionMilli = 2000;
    static constexpr uint16_t kMaxContentLen               = OPENTHREAD_CONFIG_DTLS_MAX_CONTENT_LEN;

#if !OPENTHREAD_CONFIG_TLS_API_ENABLE
    static constexpr uint16_t kApplicationDataMaxLength = 1152;
#else
    static constexpr uint16_t         kApplicationDataMaxLength = OPENTHREAD_CONFIG_DTLS_APPLICATION_DATA_MAX_LENGTH;
#endif

    enum State : uint8_t
    {
        kStateDisconnected,
        kStateInitializing,
        kStateConnecting,
        kStateConnected,
        kStateDisconnecting,
    };

    explicit SecureSession(SecureTransport &aTransport);

    bool  IsDisconnected(void) const { return mState == kStateDisconnected; }
    bool  IsInitializing(void) const { return mState == kStateInitializing; }
    bool  IsConnecting(void) const { return mState == kStateConnecting; }
    bool  IsDisconnecting(void) const { return mState == kStateDisconnecting; }
    bool  IsConnectingOrConnected(void) const { return mState == kStateConnecting || mState == kStateConnected; }
    void  SetState(State aState);
    bool  Matches(const Ip6::MessageInfo &aInfo) { return mMessageInfo.HasSamePeerAddrAndPort(aInfo); }
    void  HandleTransportReceive(Message &aMessage, const Ip6::MessageInfo &aMessageInfo);
    Error Setup(void);
    void  Disconnect(ConnectEvent aEvent);
    void  HandleTimer(TimeMilli aNow);
    void  Process(void);
    void  FreeMbedtls(void);

    static int  HandleMbedtlsGetTimer(void *aContext);
    int         HandleMbedtlsGetTimer(void);
    static void HandleMbedtlsSetTimer(void *aContext, uint32_t aIntermediate, uint32_t aFinish);
    void        HandleMbedtlsSetTimer(uint32_t aIntermediate, uint32_t aFinish);
    static int  HandleMbedtlsReceive(void *aContext, unsigned char *aBuf, size_t aLength);
    int         HandleMbedtlsReceive(unsigned char *aBuf, size_t aLength);
    static int  HandleMbedtlsTransmit(void *aContext, const unsigned char *aBuf, size_t aLength);
    int         HandleMbedtlsTransmit(const unsigned char *aBuf, size_t aLength);

    static bool IsMbedtlsHandshakeOver(mbedtls_ssl_context *aSslContext);

#if OT_SHOULD_LOG_AT(OT_LOG_LEVEL_INFO)
    static const char *StateToString(State aState);
#endif

    bool                       mTimerSet : 1;
    State                      mState;
    Message::SubType           mMessageSubType;
    ConnectEvent               mConnectEvent;
    TimeMilli                  mTimerIntermediate;
    TimeMilli                  mTimerFinish;
    SecureTransport           &mTransport;
    Message                   *mReceiveMessage;
    Ip6::MessageInfo           mMessageInfo;
    Callback<ConnectedHandler> mConnectedCallback;
    Callback<ReceiveHandler>   mReceiveCallback;
    mbedtls_ssl_config         mConf;
    mbedtls_ssl_context        mSsl;
#if defined(MBEDTLS_SSL_SRV_C) && defined(MBEDTLS_SSL_COOKIE_C)
    mbedtls_ssl_cookie_ctx mCookieCtx;
#endif
};

/**
 * Represents a secure transport, used as base class for `Dtls` and `Tls`.
 */
class SecureTransport : public InstanceLocator
{
    friend class SecureSession;

public:
    static constexpr uint8_t kPskMaxLength = 32; ///< Maximum PSK length.

    /**
     * Pointer is called to send encrypted message.
     *
     * @param[in]  aContext      A pointer to arbitrary context information.
     * @param[in]  aMessage      A reference to the message to send.
     * @param[in]  aMessageInfo  A reference to the message info associated with @p aMessage.
     */
    typedef Error (*TransportCallback)(void *aContext, ot::Message &aMessage, const Ip6::MessageInfo &aMessageInfo);

    /**
     * Callback to notify when the socket is automatically closed due to reaching the maximum number of connection
     * attempts (set from `SetMaxConnectionAttempts()`).
     *
     * @param[in] aContext    A pointer to arbitrary context information.
     */
    typedef void (*AutoCloseCallback)(void *aContext);

#if OPENTHREAD_CONFIG_TLS_API_ENABLE
    /**
     * Represents an API extension for a `SecureTransport` (DTLS or TLS).
     *
     * The `Extension` provides support for additional cipher suites along with related methods to configure them.
     * This class decouples this functionality from the common `SecureTransport` object, allowing this to be added
     * to any class.
     *
     * The general pattern to use the `Extension` class is to have it be inherited by classes that want to provide the
     * same methods for configuring ciphers (e.g. `SetPreSharedKey()` or `SetCertificate()`). An `Extension` should
     * then be associated with a `SecureTransport` (or any of its subclasses).
     */
    class Extension
    {
        friend SecureTransport;
        friend SecureSession;

    public:
#ifdef MBEDTLS_KEY_EXCHANGE_PSK_ENABLED
        /**
         * Sets the Pre-Shared Key (PSK) for sessions-identified by a PSK.
         *
         * DTLS mode "PSK with AES 128 CCM 8" for Application CoAPS.
         *
         * @param[in]  aPsk          A pointer to the PSK.
         * @param[in]  aPskLength    The PSK char length.
         * @param[in]  aPskIdentity  The Identity Name for the PSK.
         * @param[in]  aPskIdLength  The PSK Identity Length.
         */
        void SetPreSharedKey(const uint8_t *aPsk,
                             uint16_t       aPskLength,
                             const uint8_t *aPskIdentity,
                             uint16_t       aPskIdLength);
#endif

#ifdef MBEDTLS_KEY_EXCHANGE_ECDHE_ECDSA_ENABLED
        /**
         * Sets a reference to the own x509 certificate with corresponding private key.
         *
         * DTLS mode "ECDHE ECDSA with AES 128 CCM 8" for Application CoAPS.
         *
         * @param[in]  aX509Certificate   A pointer to the PEM formatted X509 certificate.
         * @param[in]  aX509CertLength    The length of certificate.
         * @param[in]  aPrivateKey        A pointer to the PEM formatted private key.
         * @param[in]  aPrivateKeyLength  The length of the private key.
         */
        void SetCertificate(const uint8_t *aX509Certificate,
                            uint32_t       aX509CertLength,
                            const uint8_t *aPrivateKey,
                            uint32_t       aPrivateKeyLength);

        /**
         * Sets the trusted top level CAs. It is needed for validate the certificate of the peer.
         *
         * DTLS mode "ECDHE ECDSA with AES 128 CCM 8" for Application CoAPS.
         *
         * @param[in]  aX509CaCertificateChain  A pointer to the PEM formatted X509 CA chain.
         * @param[in]  aX509CaCertChainLength   The length of chain.
         */
        void SetCaCertificateChain(const uint8_t *aX509CaCertificateChain, uint32_t aX509CaCertChainLength);

        /**
         * Extracts public key from it's own certificate.
         *
         * @returns Public key from own certificate in form of entire ASN.1 field.
         */
        const mbedtls_asn1_buf &GetOwnPublicKey(void) const { return mEcdheEcdsaInfo.mOwnCert.pk_raw; }

#endif // MBEDTLS_KEY_EXCHANGE_ECDHE_ECDSA_ENABLED

#if defined(MBEDTLS_BASE64_C) && defined(MBEDTLS_SSL_KEEP_PEER_CERTIFICATE)
        /**
         * Returns the peer x509 certificate base64 encoded.
         *
         * DTLS mode "ECDHE ECDSA with AES 128 CCM 8" for Application CoAPS.
         *
         * @param[out]  aPeerCert        A pointer to the base64 encoded certificate buffer.
         * @param[out]  aCertLength      The length of the base64 encoded peer certificate.
         * @param[in]   aCertBufferSize  The buffer size of aPeerCert.
         *
         * @retval kErrorInvalidState   Not connected yet.
         * @retval kErrorNone           Successfully get the peer certificate.
         * @retval kErrorNoBufs         Can't allocate memory for certificate.
         */
        Error GetPeerCertificateBase64(unsigned char *aPeerCert, size_t *aCertLength, size_t aCertBufferSize);
#endif // defined(MBEDTLS_BASE64_C) && defined(MBEDTLS_SSL_KEEP_PEER_CERTIFICATE)

#if defined(MBEDTLS_SSL_KEEP_PEER_CERTIFICATE)
        /**
         * Returns an attribute value identified by its OID from the subject
         * of the peer x509 certificate. The peer OID is provided in binary format.
         * The attribute length is set if the attribute was successfully read or zero
         * if unsuccessful. The ASN.1 type as is set as defineded in the ITU-T X.690 standard
         * if the attribute was successfully read.
         *
         * @param[in]     aOid                A pointer to the OID to be found.
         * @param[in]     aOidLength          The length of the OID.
         * @param[out]    aAttributeBuffer    A pointer to the attribute buffer.
         * @param[in,out] aAttributeLength    On input, the size the max size of @p aAttributeBuffer.
         *                                    On output, the length of the attribute written to the buffer.
         * @param[out]    aAsn1Type           A pointer to the ASN.1 type of the attribute written to the buffer.
         *
         * @retval kErrorInvalidState   Not connected yet.
         * @retval kErrorInvalidArgs    Invalid attribute length.
         * @retval kErrorNone           Successfully read attribute.
         * @retval kErrorNoBufs         Insufficient memory for storing the attribute value.
         */
        Error GetPeerSubjectAttributeByOid(const char *aOid,
                                           size_t      aOidLength,
                                           uint8_t    *aAttributeBuffer,
                                           size_t     *aAttributeLength,
                                           int        *aAsn1Type);

        /**
         * Returns an attribute value for the OID 1.3.6.1.4.1.44970.x from the v3 extensions of
         * the peer x509 certificate, where the last digit x is set to aThreadOidDescriptor.
         * The attribute length is set if the attribute was successfully read or zero if unsuccessful.
         * Requires a connection to be active.
         *
         * @param[in]      aThreadOidDescriptor  The last digit of the Thread attribute OID.
         * @param[out]     aAttributeBuffer      A pointer to the attribute buffer.
         * @param[in,out]  aAttributeLength      On input, the size the max size of @p aAttributeBuffer.
         *                                           On output, the length of the attribute written to the buffer.
         *
         * @retval kErrorNone             Successfully read attribute.
         * @retval kErrorInvalidArgs      Invalid attribute length.
         * @retval kErrorNotFound         The requested attribute was not found.
         * @retval kErrorNoBufs           Insufficient memory for storing the attribute value.
         * @retval kErrorInvalidState     Not connected yet.
         * @retval kErrorNotImplemented   The value of aThreadOidDescriptor is >127.
         * @retval kErrorParse            The certificate extensions could not be parsed.
         */
        Error GetThreadAttributeFromPeerCertificate(int      aThreadOidDescriptor,
                                                    uint8_t *aAttributeBuffer,
                                                    size_t  *aAttributeLength);
#endif // defined(MBEDTLS_SSL_KEEP_PEER_CERTIFICATE)

        /**
         * Returns an attribute value for the OID 1.3.6.1.4.1.44970.x from the v3 extensions of
         * the own x509 certificate, where the last digit x is set to aThreadOidDescriptor.
         * The attribute length is set if the attribute was successfully read or zero if unsuccessful.
         * Requires a connection to be active.
         *
         * @param[in]      aThreadOidDescriptor  The last digit of the Thread attribute OID.
         * @param[out]     aAttributeBuffer      A pointer to the attribute buffer.
         * @param[in,out]  aAttributeLength      On input, the size the max size of @p aAttributeBuffer.
         *                                       On output, the length of the attribute written to the buffer.
         *
         * @retval kErrorNone             Successfully read attribute.
         * @retval kErrorInvalidArgs      Invalid attribute length.
         * @retval kErrorNotFound         The requested attribute was not found.
         * @retval kErrorNoBufs           Insufficient memory for storing the attribute value.
         * @retval kErrorInvalidState     Not connected yet.
         * @retval kErrorNotImplemented   The value of aThreadOidDescriptor is >127.
         * @retval kErrorParse            The certificate extensions could not be parsed.
         */
        Error GetThreadAttributeFromOwnCertificate(int      aThreadOidDescriptor,
                                                   uint8_t *aAttributeBuffer,
                                                   size_t  *aAttributeLength);

        /**
         * Set the authentication mode for a connection.
         *
         * Disable or enable the verification of peer certificate.
         * Must called before start.
         *
         * @param[in]  aVerifyPeerCertificate  true, if the peer certificate should verify.
         */
        void SetSslAuthMode(bool aVerifyPeerCertificate)
        {
            mSecureTransport.mVerifyPeerCertificate = aVerifyPeerCertificate;
        }

    protected:
        explicit Extension(SecureTransport &aSecureTransport)
            : mSecureTransport(aSecureTransport)
        {
        }

    private:
#if defined(MBEDTLS_KEY_EXCHANGE_ECDHE_ECDSA_ENABLED)
        struct EcdheEcdsaInfo : public Clearable<EcdheEcdsaInfo>
        {
            EcdheEcdsaInfo(void) { Clear(); }
            void Init(void);
            void Free(void);
            int  SetSecureKeys(mbedtls_ssl_config &aConfig);

            const uint8_t     *mCaChainSrc;
            const uint8_t     *mOwnCertSrc;
            const uint8_t     *mPrivateKeySrc;
            uint32_t           mOwnCertLength;
            uint32_t           mCaChainLength;
            uint32_t           mPrivateKeyLength;
            mbedtls_x509_crt   mCaChain;
            mbedtls_x509_crt   mOwnCert;
            mbedtls_pk_context mPrivateKey;
        };
#endif

#if defined(MBEDTLS_KEY_EXCHANGE_PSK_ENABLED)
        struct PskInfo : public Clearable<PskInfo>
        {
            PskInfo(void) { Clear(); }
            int SetSecureKeys(mbedtls_ssl_config &aConfig) const;

            const uint8_t *mPreSharedKey;
            const uint8_t *mPreSharedKeyIdentity;
            uint16_t       mPreSharedKeyLength;
            uint16_t       mPreSharedKeyIdLength;
        };
#endif

        int   SetApplicationSecureKeys(mbedtls_ssl_config &aConfig);
        Error GetThreadAttributeFromCertificate(const mbedtls_x509_crt *aCert,
                                                int                     aThreadOidDescriptor,
                                                uint8_t                *aAttributeBuffer,
                                                size_t                 *aAttributeLength);

        SecureTransport &mSecureTransport;
#if defined(MBEDTLS_KEY_EXCHANGE_ECDHE_ECDSA_ENABLED)
        EcdheEcdsaInfo mEcdheEcdsaInfo;
#endif
#if defined(MBEDTLS_KEY_EXCHANGE_PSK_ENABLED)
        PskInfo mPskInfo;
#endif
    };
#endif // OPENTHREAD_CONFIG_TLS_API_ENABLE

    /**
     * Opens the socket.
     *
     * @param[in]  aReceiveHandler      A pointer to a function that is called to receive payload.
     * @param[in]  aConnectedHandler    A pointer to a function that is called when connected or disconnected.
     * @param[in]  aContext             A pointer to arbitrary context information.
     *
     * @retval kErrorNone     Successfully opened the socket.
     * @retval kErrorAlready  The connection is already open.
     */
    Error Open(SecureSession::ReceiveHandler   aReceiveHandler,
               SecureSession::ConnectedHandler aConnectedHandler,
               void                           *aContext);

    /**
     * Sets the maximum number of allowed connection requests before socket is automatically closed.
     *
     * This method can be called when socket is closed. Otherwise `kErrorInvalidSatet` is returned.
     *
     * If @p aMaxAttempts is zero, no limit is applied and connections are allowed until the socket is closed. This is
     * the default behavior if `SetMaxConnectionAttempts()` is not called.
     *
     * @param[in] aMaxAttempts    Maximum number of allowed connection attempts.
     * @param[in] aCallback       Callback to notify when max number of attempts has reached and socket is closed.
     * @param[in] aContext        A pointer to arbitrary context to use with `AutoCloseCallback`.
     *
     * @retval kErrorNone          Successfully set the maximum allowed connection attempts and callback.
     * @retval kErrorInvalidState  Socket is not closed.
     */
    Error SetMaxConnectionAttempts(uint16_t aMaxAttempts, AutoCloseCallback aCallback, void *aContext);

    /**
     * Binds this DTLS to a UDP port.
     *
     * @param[in]  aPort              The port to bind.
     *
     * @retval kErrorNone           Successfully bound the socket.
     * @retval kErrorInvalidState   The socket is not open.
     * @retval kErrorAlready        Already bound.
     */
    Error Bind(uint16_t aPort);

    /**
     * Gets the UDP port of this session.
     *
     * @returns  UDP port number.
     */
    uint16_t GetUdpPort(void) const { return mSocket.GetSockName().GetPort(); }

    /**
     * Binds with a transport callback.
     *
     * @param[in]  aCallback  A pointer to a function for sending messages.
     * @param[in]  aContext   A pointer to arbitrary context information.
     *
     * @retval kErrorNone           Successfully bound the socket.
     * @retval kErrorInvalidState   The socket is not open.
     * @retval kErrorAlready        Already bound.
     */
    Error Bind(TransportCallback aCallback, void *aContext);

    /**
     * Indicates whether or not the secure transpose socket is closed.
     *
     * @retval TRUE   The secure transport socket closed.
     * @retval FALSE  The secure transport socket is not closed.
     */
    bool IsClosed(void) const { return !mIsOpen; }

    /**
     * Closes the socket.
     */
    void Close(void);

    /**
     * Sets the PSK.
     *
     * @param[in]  aPsk  A pointer to the PSK.
     *
     * @retval kErrorNone          Successfully set the PSK.
     * @retval kErrorInvalidArgs   The PSK is invalid.
     */
    Error SetPsk(const uint8_t *aPsk, uint8_t aPskLength);

    /**
     * Checks and handles a received message provided to the SecureTransport object. If checks based on
     * the message info and current connection state pass, the message is processed.
     *
     * @param[in]  aMessage  A reference to the message to receive.
     * @param[in]  aMessageInfo A reference to the message info associated with @p aMessage.
     */
    void HandleReceive(Message &aMessage, const Ip6::MessageInfo &aMessageInfo);

protected:
    SecureTransport(Instance &aInstance, LinkSecurityMode aLayerTwoSecurity, bool aDatagramTransport);

    void SetSession(SecureSession &aSesssion) { mSession = &aSesssion; }

#if OPENTHREAD_CONFIG_TLS_API_ENABLE
    void SetExtension(Extension &aExtension) { mExtension = &aExtension; }
#endif

private:
    static constexpr size_t kSecureTransportKeyBlockSize     = 40;
    static constexpr size_t kSecureTransportRandomBufferSize = 32;

    enum CipherSuite : uint8_t
    {
        kEcjpakeWithAes128Ccm8,
#if OPENTHREAD_CONFIG_TLS_API_ENABLE && defined(MBEDTLS_KEY_EXCHANGE_PSK_ENABLED)
        kPskWithAes128Ccm8,
#endif
#if OPENTHREAD_CONFIG_TLS_API_ENABLE && defined(MBEDTLS_KEY_EXCHANGE_ECDHE_ECDSA_ENABLED)
        kEcdheEcdsaWithAes128Ccm8,
        kEcdheEcdsaWithAes128GcmSha256,
#endif
        kUnspecifiedCipherSuite,
    };

    void DecremenetRemainingConnectionAttempts(void);
    bool HasNoRemainingConnectionAttempts(void) const;
    int  Transmit(const unsigned char    *aBuf,
                  size_t                  aLength,
                  const Ip6::MessageInfo &aMessageInfo,
                  Message::SubType        aMessageSubType);

    static void HandleMbedtlsDebug(void *aContext, int aLevel, const char *aFile, int aLine, const char *aStr);
    void        HandleMbedtlsDebug(int aLevel, const char *aFile, int aLine, const char *aStr);

#ifdef MBEDTLS_SSL_EXPORT_KEYS
#if (MBEDTLS_VERSION_NUMBER >= 0x03000000)

    static void HandleMbedtlsExportKeys(void                       *aContext,
                                        mbedtls_ssl_key_export_type aType,
                                        const unsigned char        *aMasterSecret,
                                        size_t                      aMasterSecretLen,
                                        const unsigned char         aClientRandom[32],
                                        const unsigned char         aServerRandom[32],
                                        mbedtls_tls_prf_types       aTlsPrfType);

    void HandleMbedtlsExportKeys(mbedtls_ssl_key_export_type aType,
                                 const unsigned char        *aMasterSecret,
                                 size_t                      aMasterSecretLen,
                                 const unsigned char         aClientRandom[32],
                                 const unsigned char         aServerRandom[32],
                                 mbedtls_tls_prf_types       aTlsPrfType);

#else

    static int       HandleMbedtlsExportKeys(void                *aContext,
                                             const unsigned char *aMasterSecret,
                                             const unsigned char *aKeyBlock,
                                             size_t               aMacLength,
                                             size_t               aKeyLength,
                                             size_t               aIvLength);
    int              HandleMbedtlsExportKeys(const unsigned char *aMasterSecret,
                                             const unsigned char *aKeyBlock,
                                             size_t               aMacLength,
                                             size_t               aKeyLength,
                                             size_t               aIvLength);

#endif // (MBEDTLS_VERSION_NUMBER >= 0x03000000)
#endif // MBEDTLS_SSL_EXPORT_KEYS

    static void HandleTimer(Timer &aTimer);
    void        HandleTimer(void);

    using TransportSocket = Ip6::Udp::SocketIn<SecureTransport, &SecureTransport::HandleReceive>;

#if (MBEDTLS_VERSION_NUMBER >= 0x03010000)
    static const uint16_t kGroups[];
#else
    static const mbedtls_ecp_group_id kCurves[];
#endif

#if defined(MBEDTLS_KEY_EXCHANGE__WITH_CERT__ENABLED) || defined(MBEDTLS_KEY_EXCHANGE_WITH_CERT_ENABLED)
#if (MBEDTLS_VERSION_NUMBER >= 0x03020000)
    static const uint16_t kSignatures[];
#else
    static const int kHashes[];
#endif
#endif

    static const int kCipherSuites[][2];

    bool                        mLayerTwoSecurity : 1;
    bool                        mDatagramTransport : 1;
    bool                        mIsOpen : 1;
    bool                        mIsServer : 1;
    bool                        mVerifyPeerCertificate : 1;
    CipherSuite                 mCipherSuite;
    uint8_t                     mPskLength;
    uint16_t                    mMaxConnectionAttempts;
    uint16_t                    mRemainingConnectionAttempts;
    SecureSession              *mSession;
    TransportSocket             mSocket;
    uint8_t                     mPsk[kPskMaxLength];
    TimerMilliContext           mTimer;
    Callback<AutoCloseCallback> mAutoCloseCallback;
    Callback<TransportCallback> mTransportCallback;
#if OPENTHREAD_CONFIG_TLS_API_ENABLE
    Extension *mExtension;
#endif
};

/**
 * Represents a DTLS instance.
 */
class Dtls : public SecureTransport, public SecureSession
{
public:
    /**
     * Initializes the `Dtls` object.
     *
     * @param[in]  aInstance            A reference to the OpenThread instance.
     * @param[in]  aLayerTwoSecurity    Specifies whether to use layer two security or not.
     */
    Dtls(Instance &aInstance, LinkSecurityMode aLayerTwoSecurity)
        : SecureTransport(aInstance, aLayerTwoSecurity, /* aDatagramTransport */ true)
        , SecureSession(*static_cast<SecureTransport *>(this))
    {
        SetSession(*static_cast<SecureSession *>(this));
    }
};

#if OPENTHREAD_CONFIG_COAP_SECURE_API_ENABLE

/**
 * Represents an extended DTLS instance providing `Dtls::Extension` APIs.
 */
class DtlsExtended : public Dtls
{
public:
    /**
     * Initializes the `DtlsExtended` object.
     *
     * @param[in]  aInstance            A reference to the OpenThread instance.
     * @param[in]  aLayerTwoSecurity    Specifies whether to use layer two security or not.
     * @param[in]  aExtension           An extension providing additional configuration methods.
     */
    DtlsExtended(Instance &aInstance, LinkSecurityMode aLayerTwoSecurity, Extension &aExtension)
        : Dtls(aInstance, aLayerTwoSecurity)
    {
        SetExtension(aExtension);
    }
};

#endif

#if OPENTHREAD_CONFIG_BLE_TCAT_ENABLE

/**
 * Represents a TLS instance.
 */
class Tls : public SecureTransport, public SecureSession
{
public:
    /**
     * Initializes the `Tls` object.
     *
     * @param[in]  aInstance            A reference to the OpenThread instance.
     * @param[in]  aLayerTwoSecurity    Specifies whether to use layer two security or not.
     * @param[in]  aExtension           An extension providing additional configuration methods.
     */
    Tls(Instance &aInstance, LinkSecurityMode aLayerTwoSecurity, Extension &aExtension)
        : SecureTransport(aInstance, aLayerTwoSecurity, /* aDatagramTransport */ false)
        , SecureSession(*static_cast<SecureTransport *>(this))
    {
        SetSession(*static_cast<SecureSession *>(this));
        SetExtension(aExtension);
    }
};

#endif

} // namespace MeshCoP
} // namespace ot

#endif // SECURE_TRANSPORT_HPP_
