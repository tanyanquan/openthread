/*
 *  Copyright (c) 2020, The OpenThread Authors.
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

#ifndef CSL_TX_SCHEDULER_HPP_
#define CSL_TX_SCHEDULER_HPP_

#include "openthread-core-config.h"

#if OPENTHREAD_CONFIG_MAC_CSL_TRANSMITTER_ENABLE

#include "common/locator.hpp"
#include "common/message.hpp"
#include "common/non_copyable.hpp"
#include "common/time.hpp"
#include "mac/mac.hpp"
#include "mac/mac_frame.hpp"
#include "thread/indirect_sender_frame_context.hpp"

namespace ot {

/**
 * @addtogroup core-mesh-forwarding
 *
 * @brief
 *   This module includes definitions for CSL transmission scheduling.
 *
 * @{
 */

class CslNeighbor;

/**
 * Implements CSL tx scheduling functionality.
 */
class CslTxScheduler : public InstanceLocator, private NonCopyable
{
    friend class Mac::Mac;
    friend class IndirectSender;

public:
    static constexpr uint8_t kMaxCslTriggeredTxAttempts = OPENTHREAD_CONFIG_MAC_MAX_TX_ATTEMPTS_INDIRECT_POLLS;

    /**
     * Defines all the neighbor info required for scheduling CSL transmissions.
     *
     * `CslNeighbor` publicly inherits from this class.
     */
    class NeighborInfo
    {
    public:
        uint8_t GetCslTxAttempts(void) const { return mCslTxAttempts; }
        void    IncrementCslTxAttempts(void) { mCslTxAttempts++; }
        void    ResetCslTxAttempts(void) { mCslTxAttempts = 0; }

        bool IsCslSynchronized(void) const { return mCslSynchronized && mCslPeriod > 0; }
        void SetCslSynchronized(bool aCslSynchronized) { mCslSynchronized = aCslSynchronized; }

        uint8_t GetCslChannel(void) const { return mCslChannel; }
        void    SetCslChannel(uint8_t aChannel) { mCslChannel = aChannel; }

        uint32_t GetCslTimeout(void) const { return mCslTimeout; }
        void     SetCslTimeout(uint32_t aTimeout) { mCslTimeout = aTimeout; }

        uint16_t GetCslPeriod(void) const { return mCslPeriod; }
        void     SetCslPeriod(uint16_t aPeriod) { mCslPeriod = aPeriod; }

        uint16_t GetCslPhase(void) const { return mCslPhase; }
        void     SetCslPhase(uint16_t aPhase) { mCslPhase = aPhase; }

        TimeMilli GetCslLastHeard(void) const { return mCslLastHeard; }
        void      SetCslLastHeard(TimeMilli aCslLastHeard) { mCslLastHeard = aCslLastHeard; }

        uint64_t GetLastRxTimestamp(void) const { return mLastRxTimestamp; }
        void     SetLastRxTimestamp(uint64_t aLastRxTimestamp) { mLastRxTimestamp = aLastRxTimestamp; }

    private:
        uint8_t  mCslTxAttempts : 7;   ///< Number of CSL triggered tx attempts.
        bool     mCslSynchronized : 1; ///< Indicates whether or not the child is CSL synchronized.
        uint8_t  mCslChannel;          ///< The channel the device will listen on.
        uint32_t mCslTimeout;          ///< The sync timeout, in seconds.
        uint16_t mCslPeriod; ///< CSL sampled listening period between consecutive channel samples in units of 10
                             ///< symbols (160 microseconds).

        /**
         * The time in units of 10 symbols from the first symbol of the frame
         * containing the CSL IE was transmitted until the next channel sample,
         * see IEEE 802.15.4-2015, section 6.12.2.
         *
         * The Thread standard further defines the CSL phase (see Thread 1.4,
         * section 3.2.6.3.4, also conforming to IEEE 802.15.4-2020, section
         * 6.12.2.1):
         *  * The "first symbol" from the definition SHALL be interpreted as the
         *    first symbol of the MAC Header.
         *  * "until the next channel sample":
         *     * The CSL Receiver SHALL be ready to receive when the preamble
         *       time T_pa as specified below is reached.
         *     * The CSL Receiver SHOULD be ready to receive earlier than T_pa
         *       and SHOULD stay ready to receive until after the time specified
         *       in CSL Phase, according to the implementation and accuracy
         *       expectations.
         *     * The CSL Transmitter SHALL start transmitting the first symbol
         *       of the preamble of the frame to transmit at the preamble time
         *       T_pa = (CSL-Phase-Time – 192 us) (that is, CCA must be
         *       performed before time T_pa). Here, CSL-Phase-Time is the time
         *       duration specified by the CslPhase field value (in units of 10
         *       symbol periods).
         *     * This implies that the CSL Transmitter SHALL start transmitting
         *       the first symbol of the MAC Header at the time T_mh =
         *       CSL-Phase-Time.
         *
         * Derivation of the next TX timestamp based on this definition and the
         * RX timestamp of the packet containing the CSL IE:
         *
         * Note that RX and TX timestamps are defined to point to the end of the
         * synchronization header (SHR).
         *
         * lastTmh = lastRxTimestamp + phrDuration
         *
         * nextTmh = lastTmh + symbolPeriod * 10 * (n * cslPeriod + cslPhase)
         *         = lastTmh + 160us * (n * cslPeriod + cslPhase)
         *
         * nextTxTimestamp
         *         = nextTmh - phrDuration
         *         = lastRxTimestamp + 160us * (n * cslPeriod + cslPhase)
         */
        uint16_t  mCslPhase;
        TimeMilli mCslLastHeard; ///< Radio clock time when last frame containing CSL IE was heard.
        uint64_t
            mLastRxTimestamp; ///< Radio clock time when last frame containing CSL IE was received, in microseconds.

        static_assert(kMaxCslTriggeredTxAttempts < (1 << 7), "mCslTxAttempts cannot fit max!");
    };

    /**
     * Initializes the CSL tx scheduler object.
     *
     * @param[in]  aInstance   A reference to the OpenThread instance.
     */
    explicit CslTxScheduler(Instance &aInstance);

    /**
     * Updates the next CSL transmission (finds the nearest child).
     *
     * It would then request the `Mac` to do the CSL tx. If the last CSL tx has been fired at `Mac` but hasn't been
     * done yet, and it's aborted, this method would clear current tx CSL neighbor to notify the `HandleTransmitDone`
     * that the operation has been aborted.
     */
    void Update(void);

    /**
     * Clears all the states inside `CslTxScheduler` and the related states in each child.
     */
    void Clear(void);

    /**
     * Updates the value of `mCslFrameRequestAheadUs`, based on bus speed, bus latency and `Mac::kCslRequestAhead`.
     */
    void UpdateFrameRequestAhead(void);

private:
    // Guard time in usec to add when checking delay while preparing the CSL frame for tx.
    static constexpr uint32_t kFramePreparationGuardInterval = 1500;

    typedef IndirectSenderBase::FrameContext FrameContext;

    void RescheduleCslTx(void);

    uint32_t GetNextCslTransmissionDelay(const CslNeighbor &aCslNeighbor,
                                         uint32_t          &aDelayFromLastRx,
                                         uint32_t           aAheadUs) const;

    // Callbacks from `Mac`
    Mac::TxFrame *HandleFrameRequest(Mac::TxFrames &aTxFrames);
    void          HandleSentFrame(const Mac::TxFrame &aFrame, Error aError);

    void HandleSentFrame(const Mac::TxFrame &aFrame, Error aError, CslNeighbor &aaCslNeighbor);

    uint32_t     mCslFrameRequestAheadUs;
    CslNeighbor *mCslTxNeighbor;
    Message     *mCslTxMessage;
    FrameContext mFrameContext;
};

/**
 * @}
 */

} // namespace ot

#endif // OPENTHREAD_CONFIG_MAC_CSL_TRANSMITTER_ENABLE

#endif // CSL_TX_SCHEDULER_HPP_
