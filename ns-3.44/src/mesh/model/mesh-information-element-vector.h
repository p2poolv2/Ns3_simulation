/*
 * Copyright (c) 2009 IITP RAS
 *
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Authors: Kirill Andreev <andreev@iitp.ru>
 *          Pavel Boyko <boyko.iitp.ru>
 */

#ifndef MESH_INFORMATION_ELEMENT_VECTOR_H
#define MESH_INFORMATION_ELEMENT_VECTOR_H

#include "ns3/wifi-information-element.h"

namespace ns3
{

#define IE11S_MESH_PEERING_PROTOCOL_VERSION                                                        \
    ((WifiInformationElementId)74) // to be removed (Protocol ID should be part of the Mesh Peering
                                   // Management IE)

/**
 * @brief Information element vector
 * @ingroup wifi
 *
 * Implements a vector of WifiInformationElements.
 * Information elements typically come in groups, and the
 * WifiInformationElementVector class provides a representation of a
 * series of IEs, and the facility for serialisation to and
 * deserialisation from the over-the-air format.
 */
class MeshInformationElementVector : public Header
{
  public:
    MeshInformationElementVector();
    ~MeshInformationElementVector() override;

    /**
     * @brief Get the type ID.
     * @return the object TypeId
     */
    static TypeId GetTypeId();

    TypeId GetInstanceTypeId() const override;
    uint32_t GetSerializedSize() const override;
    void Serialize(Buffer::Iterator start) const override;
    /**
     * @attention This variant should not be used but is implemented due to
     * backward compatibility reasons
     *
     * @param start buffer location to start deserializing from
     * @return number of bytes deserialized
     */
    uint32_t Deserialize(Buffer::Iterator start) override;
    /**
     * Deserialize a number of WifiInformationElements
     *
     * The size of this Header should equal start.GetDistanceFrom (end).
     *
     * @param start starting buffer location
     * @param end ending buffer location
     * @return number of bytes deserialized
     */
    uint32_t Deserialize(Buffer::Iterator start, Buffer::Iterator end) override;
    void Print(std::ostream& os) const override;

    /**
     * @brief Needed when you try to deserialize a lonely IE inside other header
     *
     * @param start is the start of the buffer
     *
     * @return deserialized bytes
     */
    uint32_t DeserializeSingleIe(Buffer::Iterator start);

    /// As soon as this is a vector, we define an Iterator
    typedef std::vector<Ptr<WifiInformationElement>>::iterator Iterator;
    /**
     * Returns Begin of the vector
     * @returns the begin of the vector
     */
    Iterator Begin();
    /**
     * Returns End of the vector
     * @returns the end of the vector
     */
    Iterator End();
    /**
     * add an IE, if maxSize has exceeded, returns false
     *
     * @param element wifi information element to add
     * @returns true is added
     */
    bool AddInformationElement(Ptr<WifiInformationElement> element);
    /**
     * vector of pointers to information elements is the body of IeVector
     *
     * @param id the element id to find
     * @returns the information element
     */
    Ptr<WifiInformationElement> FindFirst(WifiInformationElementId id) const;

    /**
     * Check if the given WifiInformationElementVectors are equivalent.
     *
     * @param a another WifiInformationElementVector
     *
     * @return true if the given WifiInformationElementVectors are equivalent,
     *         false otherwise
     */
    virtual bool operator==(const MeshInformationElementVector& a) const;

  protected:
    /**
     * typedef for a vector of WifiInformationElements.
     */
    typedef std::vector<Ptr<WifiInformationElement>> IE_VECTOR;
    /**
     * Current number of bytes
     * @returns the number of bytes
     */
    uint32_t GetSize() const;
    IE_VECTOR m_elements; //!< Information element vector
    uint16_t m_maxSize;   //!< Size in bytes (actually, max packet length)
};

} // namespace ns3

#endif
