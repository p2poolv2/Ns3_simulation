/*
 * Copyright (c) 2009, 2010 MIRKO BANCHI
 *
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Author: Mirko Banchi <mk.banchi@gmail.com>
 * Author: Tommaso Pecorella <tommaso.pecorella@unifi.it>
 */
#ifndef ORIGINATOR_BLOCK_ACK_AGREEMENT_H
#define ORIGINATOR_BLOCK_ACK_AGREEMENT_H

#include "block-ack-agreement.h"
#include "block-ack-window.h"

#include <set>

class OriginatorBlockAckWindowTest;

namespace ns3
{

class WifiMpdu;

/**
 * @ingroup wifi
 * Maintains the state and information about transmitted MPDUs with Ack Policy set to Block Ack
 * for an originator station. The state diagram is as follows:
 *
 */
// clang-format off
/**
 * \verbatim
   /------------\ send ADDBARequest ----------------
   |   START    |------------------>|   PENDING    |-------
   \------------/                   ----------------       \
         ^            receive     /        |                \
         |        ADDBAResponse  /         |                 \
         |          (failure)   v          |                  \
         |        ---------------          |                   --------------------->  ----------------
         |        |  REJECTED   |          |          receive ADDBAResponse (success)  |  ESTABLISHED |
         |        ---------------          |      no            -------------------->  ----------------
         |           receive    ^          | ADDBAResponse     /
         |        ADDBAResponse  \         |                  /
         |          (failure)     \        v                 /
         |                         ----------------         /
         |-------------------------|   NO_REPLY   |---------
           Reset after timeout     ----------------
   \endverbatim
*/
// clang-format on

/**
 *
 * See also OriginatorBlockAckAgreement::State
 */
class OriginatorBlockAckAgreement : public BlockAckAgreement
{
    /// allow BlockAckManager class access
    friend class BlockAckManager;
    /// allow OriginatorBlockAckWindowTest class access
    friend class ::OriginatorBlockAckWindowTest;

  public:
    /**
     * Constructor
     *
     * @param recipient MAC address
     * @param tid Traffic ID
     */
    OriginatorBlockAckAgreement(Mac48Address recipient, uint8_t tid);
    ~OriginatorBlockAckAgreement() override;

    /**
     * Represents the state for this agreement.
     *
     *  PENDING:
     *    If an agreement is in PENDING state it means that an ADDBARequest frame was sent to
     *    recipient in order to setup the block ack and the originator is waiting for the relative
     *    ADDBAResponse frame.
     *
     *  ESTABLISHED:
     *    The block ack is active and all packets relative to this agreement are transmitted
     *    with Ack Policy set to Block Ack.
     *
     *  NO_REPLY
     *    No reply after an ADDBA request. In this state the originator will send the rest of
     * packets in queue using normal MPDU.
     *
     *  RESET
     *    A transient state to mark the agreement for reinitialization after failed ADDBA request.
     *    Since it is a temporary state, it is not included in the state diagram above. In this
     *    state the next transmission will be treated as if the BA agreement is not created yet.
     *
     *  REJECTED (not used for now):
     *    The agreement's state becomes REJECTED if an ADDBAResponse frame is received from
     * recipient and the Status Code field is set to failure.
     */
    /// State enumeration
    enum State
    {
        PENDING,
        ESTABLISHED,
        NO_REPLY,
        RESET,
        REJECTED
    };

    /**
     * Set the current state.
     *
     * @param state to set
     */
    void SetState(State state);
    /**
     * Check if the current state of this agreement is PENDING.
     *
     * @return true if the current state of this agreement is PENDING,
     *         false otherwise
     */
    bool IsPending() const;
    /**
     * Check if the current state of this agreement is ESTABLISHED.
     *
     * @return true if the current state of this agreement is ESTABLISHED,
     *         false otherwise
     */
    bool IsEstablished() const;
    /**
     * Check if the current state of this agreement is NO_REPLY.
     *
     * @return true if the current state of this agreement is NO_REPLY,
     *         false otherwise
     */
    bool IsNoReply() const;
    /**
     * Check if the current state of this agreement is RESET.
     *
     * @return true if the current state of this agreement is RESET,
     *         false otherwise
     */
    bool IsReset() const;
    /**
     * Check if the current state of this agreement is REJECTED.
     *
     * @return true if the current state of this agreement is REJECTED,
     *         false otherwise
     */
    bool IsRejected() const;

    /**
     * Return the starting sequence number of the transmit window, if a transmit
     * window has been initialized. Otherwise, return the starting sequence number
     * stored by the BlockAckAgreement base class.
     *
     * @return the starting sequence number.
     */
    uint16_t GetStartingSequence() const override;

    /**
     * Get the distance between the current starting sequence number and the
     * given sequence number.
     *
     * @param seqNumber the given sequence number
     * @return the distance of the given sequence number from the current winstart
     */
    std::size_t GetDistance(uint16_t seqNumber) const;

    /**
     * Initialize the originator's transmit window by setting its size and starting
     * sequence number equal to the values stored by the BlockAckAgreement base class.
     */
    void InitTxWindow();

    /**
     * Advance the transmit window so as to include the transmitted MPDU, if the
     * latter is not an old packet and is beyond the current transmit window.
     *
     * @param mpdu the transmitted MPDU
     */
    void NotifyTransmittedMpdu(Ptr<const WifiMpdu> mpdu);
    /**
     * Record that the given MPDU has been acknowledged and advance the transmit
     * window if possible.
     *
     * @param mpdu the acknowledged MPDU
     */
    void NotifyAckedMpdu(Ptr<const WifiMpdu> mpdu);
    /**
     * Advance the transmit window beyond the MPDU that has been reported to
     * be discarded.
     *
     * @param mpdu the discarded MPDU
     */
    void NotifyDiscardedMpdu(Ptr<const WifiMpdu> mpdu);

    /**
     * Check whether all the MPDUs in the TX window other than the given ones have been already
     * acknowledged.
     *
     * @param seqNumbers the sequence numbers of the given MPDUs
     * @return whether all the MPDUs in the TX window other than the given ones have been already
     *         acknowledged
     */
    bool AllAckedMpdusInTxWindow(const std::set<uint16_t>& seqNumbers) const;

  private:
    /**
     * Advance the transmit window so that the starting sequence number is the
     * nearest unacknowledged MPDU.
     */
    void AdvanceTxWindow();

    State m_state;             ///< state
    BlockAckWindow m_txWindow; ///< originator's transmit window
};

} // namespace ns3

#endif /* ORIGINATOR_BLOCK_ACK_AGREEMENT_H */
