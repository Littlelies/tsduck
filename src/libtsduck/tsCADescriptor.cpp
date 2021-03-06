//----------------------------------------------------------------------------
//
// TSDuck - The MPEG Transport Stream Toolkit
// Copyright (c) 2005-2018, Thierry Lelegard
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
// 1. Redistributions of source code must retain the above copyright notice,
//    this list of conditions and the following disclaimer.
// 2. Redistributions in binary form must reproduce the above copyright
//    notice, this list of conditions and the following disclaimer in the
//    documentation and/or other materials provided with the distribution.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
// LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
// SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
// THE POSSIBILITY OF SUCH DAMAGE.
//
//----------------------------------------------------------------------------
//
//  Representation of a generic CA_descriptor.
//  Specialized classes exist, depending on the CA_system_id.
//
//----------------------------------------------------------------------------

#include "tsCADescriptor.h"
#include "tsNames.h"
#include "tsTablesDisplay.h"
#include "tsTablesFactory.h"
#include "tsxmlElement.h"
TSDUCK_SOURCE;

#define MY_XML_NAME u"CA_descriptor"
#define MY_DID ts::DID_CA

TS_XML_DESCRIPTOR_FACTORY(ts::CADescriptor, MY_XML_NAME);
TS_ID_DESCRIPTOR_FACTORY(ts::CADescriptor, ts::EDID(MY_DID));
TS_ID_DESCRIPTOR_DISPLAY(ts::CADescriptor::DisplayDescriptor, ts::EDID(MY_DID));


//----------------------------------------------------------------------------
// Default constructor:
//----------------------------------------------------------------------------

ts::CADescriptor::CADescriptor(uint16_t cas_id_, PID ca_pid_) :
    AbstractDescriptor(MY_DID, MY_XML_NAME),
    cas_id(cas_id_),
    ca_pid(ca_pid_),
    private_data()
{
    _is_valid = true;
}


//----------------------------------------------------------------------------
// Constructor from a binary descriptor
//----------------------------------------------------------------------------

ts::CADescriptor::CADescriptor(const Descriptor& desc, const DVBCharset* charset) :
    AbstractDescriptor(MY_DID, MY_XML_NAME),
    cas_id(0),
    ca_pid(PID_NULL),
    private_data()
{
    deserialize(desc, charset);
}


//----------------------------------------------------------------------------
// Serialization
//----------------------------------------------------------------------------

void ts::CADescriptor::serialize(Descriptor& desc, const DVBCharset* charset) const
{
    ByteBlockPtr bbp (new ByteBlock (2));
    CheckNonNull (bbp.pointer());

    bbp->appendUInt16 (cas_id);
    bbp->appendUInt16 (0xE000 | (ca_pid & 0x1FFF));
    bbp->append (private_data);

    (*bbp)[0] = _tag;
    (*bbp)[1] = uint8_t(bbp->size() - 2);
    Descriptor d (bbp, SHARE);
    desc = d;
}


//----------------------------------------------------------------------------
// Deserialization
//----------------------------------------------------------------------------

void ts::CADescriptor::deserialize(const Descriptor& desc, const DVBCharset* charset)
{
    _is_valid = desc.isValid() && desc.tag() == _tag && desc.payloadSize() >= 4;

    if (_is_valid) {
        const uint8_t* data = desc.payload();
        size_t size = desc.payloadSize();
        cas_id = GetUInt16 (data);
        ca_pid = GetUInt16 (data + 2) & 0x1FFF;
        private_data.copy (data + 4, size - 4);
    }
}


//----------------------------------------------------------------------------
// Static method to display a descriptor.
//----------------------------------------------------------------------------

void ts::CADescriptor::DisplayDescriptor(TablesDisplay& display, DID did, const uint8_t* data, size_t size, int indent, TID tid, PDS pds)
{
    std::ostream& strm(display.out());

    if (size >= 4) {
        const std::string margin(indent, ' ');

        // Extract common part
        uint16_t sysid = GetUInt16(data);
        uint16_t pid = GetUInt16(data + 2) & 0x1FFF;
        const UChar* const dtype = tid == TID_CAT ? u"EMM" : (tid == TID_PMT ? u"ECM" : u"CA");
        data += 4; size -= 4;

        strm << margin << UString::Format(u"CA System Id: %s, %s PID: %d (0x%X)", {names::CASId(sysid, names::FIRST), dtype, pid, pid}) << std::endl;

        // CA private part.
        if (size > 0) {
            strm << margin << "Private CA data:" << std::endl
                 << UString::Dump(data, size, UString::HEXA | UString::ASCII | UString::OFFSET, indent);
            data += size; size = 0;
        }
    }

    display.displayExtraData(data, size, indent);
}


//----------------------------------------------------------------------------
// XML serialization
//----------------------------------------------------------------------------

void ts::CADescriptor::buildXML(xml::Element* root) const
{
    root->setIntAttribute(u"CA_system_id", cas_id, true);
    root->setIntAttribute(u"CA_PID", ca_pid, true);
    if (!private_data.empty()) {
        root->addElement(u"private_data")->addHexaText(private_data);
    }
}


//----------------------------------------------------------------------------
// XML deserialization
//----------------------------------------------------------------------------

void ts::CADescriptor::fromXML(const xml::Element* element)
{
    _is_valid =
        checkXMLName(element) &&
        element->getIntAttribute<uint16_t>(cas_id, u"CA_system_id", true, 0, 0x0000, 0xFFFF) &&
        element->getIntAttribute<PID>(ca_pid, u"CA_PID", true, 0, 0x0000, 0x1FFF) &&
        element->getHexaTextChild(private_data, u"private_data", false, 0, MAX_DESCRIPTOR_SIZE - 4);
}
