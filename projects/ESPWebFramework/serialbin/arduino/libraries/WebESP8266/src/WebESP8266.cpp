/*
WebESP8266.cpp
Binary Bidirectional Protocol for ESP8266

Created by Fabrizio Di Vittorio (fdivitto2013@gmail.com)
Copyright (c) 2015 Fabrizio Di Vittorio.
All rights reserved.

This library is free software; you can redistribute it and/or
modify it under the terms of the GNU Lesser General Public
License as published by the Free Software Foundation; either
version 2.1 of the License, or (at your option) any later version.

This library is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
Lesser General Public License for more details.

You should have received a copy of the GNU Lesser General Public
License along with this library; if not, write to the Free Software
Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA

The latest version of this library can always be found at
https://github.com/fdivitto/ESPWebFramework
*/

// 
// Includes
// 
#include <Arduino.h>
#include "WebESP8266.h"

#include <avr/pgmspace.h>




// Protocol version
static uint8_t const  PROTOCOL_VERSION     = 1;

// Timings, etc...
static uint32_t const INTRA_MSG_TIMEOUT    = 200;
static uint32_t const WAIT_MSG_TIMEOUT     = 2000;
static uint32_t const PUTACK_TIMEOUT       = 200;
static uint32_t const GETACK_TIMEOUT       = 2000;
static uint32_t const ACKMSG_QUEUE_LENGTH  = 2;
static uint32_t const MAX_RESEND_COUNT     = 3;	
static uint32_t const MAX_DATA_SIZE        = 256;

// Commands
static uint8_t const CMD_ACK                 = 0;
static uint8_t const CMD_READY               = 1;
static uint8_t const CMD_IOCONF              = 2;
static uint8_t const CMD_IOSET               = 3;
static uint8_t const CMD_IOGET               = 4;
static uint8_t const CMD_IOASET              = 5;
static uint8_t const CMD_IOAGET              = 6;
static uint8_t const CMD_GETHTTPHANDLEDPAGES = 7;
static uint8_t const CMD_HTTPREQUEST         = 8;

// CMD_HTTPREQUEST - method
static uint8_t const HTTPSENDMETHOD_UNSUPPORTED = 0;
static uint8_t const HTTPSENDMETHOD_GET         = 1;
static uint8_t const HTTPSENDMETHOD_POST        = 2;
static uint8_t const HTTPSENDMETHOD_HEAD        = 3;


// Strings
static char const STR_BINPRORDY[] PROGMEM  = "BINPRORDY";

// calculates time difference in milliseconds, taking into consideration the time overflow
// note: time1 must be less than time2 (time1 < time2)
inline uint32_t millisDiff(uint32_t time1, uint32_t time2)
{
    if (time1 > time2)
        // overflow
        return 0xFFFFFFFF - time1 + time2;
    else
        return time2 - time1;
}


///////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////
// SoftTimeOut

class SoftTimeOut
{
    public:
        SoftTimeOut(uint32_t time)
            : m_timeOut(time), m_startTime(millis())
        {
        }

        operator bool()
        {
            return millisDiff(m_startTime, millis()) > m_timeOut;
        }
        
        void reset(uint32_t time)
        {
            m_timeOut   = time;
            m_startTime = millis();				
        }

    private:
        uint32_t m_timeOut;
        uint32_t m_startTime;
};	



///////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////
// HTTPFields

// data contains a set of key/value zero terminated strings:
//   keyA\0valueA\0keyB\0valueB\0....
HTTPFields::HTTPFields(char const* data, uint8_t itemsCount) 
    : m_data(data), m_itemsCount(itemsCount)
{
}


HTTPFields::HTTPFields()
    : m_data(NULL), m_itemsCount(0)
{
}
    

void HTTPFields::reset(char const* data, uint8_t itemsCount)
{
    m_data       = data;
    m_itemsCount = itemsCount;
}

    
uint8_t HTTPFields::itemsCount()
{
    return m_itemsCount;
}


// note: doesn't validate index
char const* HTTPFields::getkey(uint8_t index)
{
    char const* key = m_data;
    for (uint8_t i = 0; i != index; ++i)
        key = strchrnul(strchrnul(key, 0) + 1, 0) + 1;
    return key;
}


// get value at specified index
char const* HTTPFields::operator[](uint8_t index)
{
    char const* value = strchrnul(m_data, 0) + 1;
    for (uint8_t i = 0; i != index; ++i)
        value = strchrnul(strchrnul(value, 0) + 1, 0) + 1;
    return value;
}


// get value for specified key or NULL if not found
char const* HTTPFields::operator[](char const* key)
{
    char const* curkey = m_data;
    for (uint8_t i = 0; i != m_itemsCount; ++i)
    {
        if (strcmp(curkey, key) == 0)
            return strchrnul(curkey, 0) + 1;
        curkey = strchrnul(strchrnul(curkey, 0) + 1, 0) + 1;
    }
    return NULL;
}


// ret. required size (in bytes) to contain all fields (included ending zeroes)
uint16_t HTTPFields::calcBufferSize()
{
    char const* key = m_data;
    for (uint8_t i = 0; i != m_itemsCount; ++i)
        key = strchrnul(strchrnul(key, 0) + 1, 0) + 1;
    return key - m_data;
}




///////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////
// HTTPResponse

HTTPResponse::HTTPResponse()
    : m_contentItems(NULL), m_headerItems(NULL)
{
}


template <typename T>
void freelist(T* item)
{
    while (item)
    {
        if (item->storage == HTTPResponse::HeapToFree)
            free((void*)item->data);
        T* nitem = item->next;
        delete item;
        item = nitem;
    }
}

HTTPResponse::~HTTPResponse()
{
    freelist(m_contentItems);
    freelist(m_headerItems);
}


// one if HTTPSTATUS_xxx constants
void HTTPResponse::setStatus(uint8_t status)
{
    m_status = status;
}


uint8_t HTTPResponse::getStatus()
{
    return m_status;
}


template <typename T>
T* getLast(T* item)
{
    while (item->next)
        item = item->next;
    return item;
}


void HTTPResponse::addHeaderItem(HeaderItem* item)
{
    if (!m_headerItems)
        m_headerItems = item;
    else
        getLast(m_headerItems)->next = item;
}


void HTTPResponse::addHeader(PGM_P key, char const* value, bool copy)
{
    addHeaderItem( new HeaderItem(NULL, 
                                  copy? HTTPResponse::HeapToFree : HTTPResponse::Heap, 
                                  key, 
                                  copy? strdup(value) : value) 
                 );
}


void HTTPResponse::addHeader_P(PGM_P key, PGM_P value)
{
    addHeaderItem( new HeaderItem(NULL, HTTPResponse::Flash, key, value) );
}


void HTTPResponse::addContentItem(ContentItem* item)
{
    if (!m_contentItems)
        m_contentItems = item;
    else
        getLast(m_contentItems)->next = item;
}


void HTTPResponse::addContent(char const* string, bool copy)
{
    addContentItem( new ContentItem(NULL,
                                    copy? HTTPResponse::HeapToFree : HTTPResponse::Heap,
                                    copy? strdup(string) : string,
                                    strlen(string) + 1)
                  );
}


void HTTPResponse::addContent_P(PGM_P string)
{
    addContentItem( new ContentItem(NULL, HTTPResponse::Flash, string, strlen_P(string) + 1) );
}


// not memory checked!
void* memdup(void const* src, uint32_t len)
{
    void* dst = malloc(len);
    memcpy(dst, src, len);
    return dst;
}


void HTTPResponse::addContent(void const* data, uint16_t length, bool copy)
{
    addContentItem( new ContentItem(NULL,
                                    copy? HTTPResponse::HeapToFree : HTTPResponse::Heap,
                                    copy? memdup(data, length) : data,
                                    length)
                  );
}


void HTTPResponse::addContent_P(PGM_P data, uint16_t length)
{
    addContentItem( new ContentItem(NULL, HTTPResponse::Flash, data, length) );
}


void HTTPResponse::addContent(uint32_t value)
{
    char str[12];
    addContent(ultoa(value, str, 10), true);    
}


void HTTPResponse::addContent(float value, int8_t width, uint8_t prec)
{
    char str[abs(width) + prec + 3];    // todo: verify buf size formula!
    addContent(dtostrf(value, width, prec, str), true);
}


uint8_t HTTPResponse::calcHeadersFieldsCount()
{
    uint8_t count = 0;
    for (HeaderItem* item = m_headerItems; item; item = item->next)
        ++count;
    return count;
}


uint16_t HTTPResponse::calcHeadersBufferSize()
{
    uint16_t len = 0;
    for (HeaderItem* item = m_headerItems; item; item = item->next)
        len += 2 + strlen_P(item->key) + item->storage == HTTPResponse::Flash? strlen_P((PGM_P)item->data) : strlen((char const*)item->data);
    return len;
}


uint8_t* HTTPResponse::copyHeadersToBuffer(uint8_t* dest)
{
    for (HeaderItem* item = m_headerItems; item; item = item->next)
    {
        strcpy_P((char*)dest, item->key);
        dest += strlen_P(item->key) + 1;
        if (item->storage == HTTPResponse::Flash)
        {
            strcpy_P((char*)dest, (char*)item->data);
            dest += strlen_P((char*)item->data) + 1;
        }
        else
        {
            strcpy((char*)dest, (char*)item->data);
            dest += strlen((char*)item->data) + 1;
        }
    }
    return dest;
}


uint16_t HTTPResponse::calcContentBufferSize()
{
    uint16_t len = 0;
    for (ContentItem* item = m_contentItems; item; item = item->next)
        len += item->dataLength;
    return len;
}


uint8_t* HTTPResponse::copyContentToBuffer(uint8_t* dest)
{
    for (ContentItem* item = m_contentItems; item; item = item->next)
    {
        if (item->storage == HTTPResponse::Flash)
            memcpy_P(dest, item->data, item->dataLength);
        else
            memcpy(dest, item->data, item->dataLength);
        dest += item->dataLength;
    }
    return dest;
}



///////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////
// Message

WebESP8266::Message::Message()
    : valid(false), ID(0), command(0), dataSize(0), data(NULL)
{
}


WebESP8266::Message::Message(uint8_t ID_, uint8_t command_, uint16_t dataSize_)
    : valid(true), ID(ID_), command(command_), dataSize(dataSize_), data(dataSize_? new uint8_t[dataSize_] : NULL)
{
}


// warn: memory must be explicitly delete using freeData(). Don't create a destructor to free data!
void WebESP8266::Message::freeData()
{
    if (data != NULL)
    {
        delete[] data;
        data = NULL;
    }
}



///////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////
// Message_ACK

// base for ACK messages 
struct Message_ACK
{
    static uint16_t const SIZE = 1;
    
    uint8_t& ackID;
    
    // used to decode message
    Message_ACK(WebESP8266::Message* msg)
        : ackID(msg->data[0])
    {
    }
    // used to encode message
    Message_ACK(WebESP8266::Message* msg, uint8_t ackID_)
        : ackID(msg->data[0])
    {
        ackID = ackID_;
    }
};



///////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////
// Message_CMD_READY

struct Message_CMD_READY
{
    static uint16_t const SIZE = 12;
    
    uint8_t& protocolVersion;
    uint8_t& platform;
    char*    magicString;
    
    // used to decode message
    Message_CMD_READY(WebESP8266::Message* msg)
        : protocolVersion(msg->data[0]), 
          platform(msg->data[1]), 
          magicString((char*)msg->data + 2)
    {
    }
    // used to encode message
    Message_CMD_READY(WebESP8266::Message* msg, uint8_t protocolVersion_, uint8_t platform_, PGM_P magicString_)
        : protocolVersion(msg->data[0]), 
          platform(msg->data[1]), 
          magicString((char*)msg->data + 2)
    {
        protocolVersion = protocolVersion_;
        platform        = platform_;
        strcpy_P(magicString, magicString_);
    }
    
};



///////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////
// Message_CMD_READY_ACK

struct Message_CMD_READY_ACK : Message_ACK
{
    static uint16_t const SIZE = Message_ACK::SIZE + 12;
    
    uint8_t& protocolVersion;
    uint8_t& platform;
    char*    magicString;
    
    // used to decode message
    Message_CMD_READY_ACK(WebESP8266::Message* msg)
            : Message_ACK(msg), 
              protocolVersion(msg->data[Message_ACK::SIZE + 0]), 
              platform(msg->data[Message_ACK::SIZE + 1]), 
              magicString((char*)msg->data + Message_ACK::SIZE + 2)
    {
    }
    // used to encode message
    Message_CMD_READY_ACK(WebESP8266::Message* msg, uint8_t ackID_, uint8_t protocolVersion_, uint8_t platform_, PGM_P magicString_)
            : Message_ACK(msg, ackID_), 
              protocolVersion(msg->data[Message_ACK::SIZE + 0]), 
              platform(msg->data[Message_ACK::SIZE + 1]), 
              magicString((char*)msg->data + Message_ACK::SIZE + 2)
    {
        protocolVersion = protocolVersion_;
        platform        = platform_;
        strcpy_P(magicString, magicString_);
    }			
};



///////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////
// Message_CMD_IOCONF

struct Message_CMD_IOCONF
{
    static uint16_t const SIZE = 2;
    
    uint8_t& pin;
    uint8_t& flags;
    
    // used to decode message
    Message_CMD_IOCONF(WebESP8266::Message* msg)
        : pin(msg->data[0]), 
          flags(msg->data[1])
    {
    }
    // used to encode message
    Message_CMD_IOCONF(WebESP8266::Message* msg, uint8_t pin_, uint8_t flags_)
        : pin(msg->data[0]), 
          flags(msg->data[1])
    {
        pin   = pin_;
        flags = flags_;
    }			
};



///////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////
// Message_CMD_IOSET

struct Message_CMD_IOSET
{
    static uint16_t const SIZE = 2;
    
    uint8_t& pin;
    uint8_t& state;
    
    // used to decode message
    Message_CMD_IOSET(WebESP8266::Message* msg)
        : pin(msg->data[0]), 
          state(msg->data[1])
    {
    }
    // used to encode message
    Message_CMD_IOSET(WebESP8266::Message* msg, uint8_t pin_, uint8_t state_)
        : pin(msg->data[0]), 
          state(msg->data[1])
    {
        pin   = pin_;
        state = state_;
    }			
};



///////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////
// Message_CMD_IOGET

struct Message_CMD_IOGET
{
    static uint16_t const SIZE = 1;
    
    uint8_t& pin;
    
    // used to decode message
    Message_CMD_IOGET(WebESP8266::Message* msg)
        : pin(msg->data[0])
    {
    }
    // used to encode message
    Message_CMD_IOGET(WebESP8266::Message* msg, uint8_t pin_)
        : pin(msg->data[0])
    {
        pin   = pin_;
    }			
};



///////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////
// Message_CMD_IOGET_ACK

struct Message_CMD_IOGET_ACK : Message_ACK
{
    static uint16_t const SIZE = Message_ACK::SIZE + 1;
    
    uint8_t& state;
    
    // used to decode message
    Message_CMD_IOGET_ACK(WebESP8266::Message* msg)
        : Message_ACK(msg), 
          state(msg->data[Message_ACK::SIZE + 0])
    {
    }
    // used to encode message
    Message_CMD_IOGET_ACK(WebESP8266::Message* msg, uint8_t ackID_, uint8_t state_)
        : Message_ACK(msg, ackID_), 
          state(msg->data[Message_ACK::SIZE + 0])
    {
        state = state_;
    }			
};



///////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////
// Message_CMD_IOASET

struct Message_CMD_IOASET
{
    static uint16_t const SIZE = 3;
    
    uint8_t&  pin;
    uint16_t& state;
    
    // used to decode message
    Message_CMD_IOASET(WebESP8266::Message* msg)
        : pin(msg->data[0]), 
          state(*(uint16_t*)(msg->data + 1))
    {
    }
    // used to encode message
    Message_CMD_IOASET(WebESP8266::Message* msg, uint8_t pin_, uint16_t state_)
        : pin(msg->data[0]), 
          state(*(uint16_t*)(msg->data + 1))
    {
        pin   = pin_;
        state = state_;
    }			
};



///////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////
// Message_CMD_IOAGET

struct Message_CMD_IOAGET
{
    static uint16_t const SIZE = 1;
    
    uint8_t& pin;
    
    // used to decode message
    Message_CMD_IOAGET(WebESP8266::Message* msg)
        : pin(msg->data[0])
    {
    }
    // used to encode message
    Message_CMD_IOAGET(WebESP8266::Message* msg, uint8_t pin_)
        : pin(msg->data[0])
    {
        pin   = pin_;
    }			
};



///////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////
// Message_CMD_IOAGET_ACK

struct Message_CMD_IOAGET_ACK : Message_ACK
{
    static uint16_t const SIZE = Message_ACK::SIZE + 2;
    
    uint16_t& state;
    
    // used to decode message
    Message_CMD_IOAGET_ACK(WebESP8266::Message* msg)
        : Message_ACK(msg), 
          state(*(uint16_t*)(msg->data + Message_ACK::SIZE + 0))
    {
    }
    // used to encode message
    Message_CMD_IOAGET_ACK(WebESP8266::Message* msg, uint8_t ackID_, uint16_t state_)
        : Message_ACK(msg, ackID_), 
          state(*(uint16_t*)(msg->data + Message_ACK::SIZE + 0))
    {
        state = state_;
    }			
};


    
//////////////////////////////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////////////////////////////
// WebESP8266


WebESP8266::WebESP8266()
	: _stream(NULL),
	  _recvID(255),
	  _sendID(0),
	  _isReady(false),
	  _platform(PLATFORM_BASELINE)
{
}


void WebESP8266::begin(Stream& stream)
{
	_stream = &stream;
	send_CMD_READY();
}


void WebESP8266::setupWebRoutes(WebRoute* webRoutes, uint8_t count)
{
    _webRoutes      = webRoutes;
    _webRoutesCount = count;
}


bool WebESP8266::isReady()
{
	return _isReady;
}


bool WebESP8266::checkReady()
{
	if (!isReady())
		send_CMD_READY();
	return isReady();
}


uint8_t WebESP8266::getPlatform()
{
	checkReady();
	return _platform;
}


void WebESP8266::yield()
{
	if (_stream->available() > 0)
	{
		Message msg = receive();
		if (msg.valid)
		{
			if (msg.command != CMD_ACK)
				processMessage(&msg);
		}
	}
}


// must not process CMD_ACK messages
void WebESP8266::processMessage(Message* msg)
{
	switch (msg->command)
	{
		case CMD_READY:
			handle_CMD_READY(msg);
			break;
		case CMD_IOCONF:
			handle_CMD_IOCONF(msg);
			break;
		case CMD_IOSET:
			handle_CMD_IOSET(msg);
			break;
		case CMD_IOGET:
			handle_CMD_IOGET(msg);
			break;
		case CMD_IOASET:
			handle_CMD_IOASET(msg);
			break;
		case CMD_IOAGET:
			handle_CMD_IOAGET(msg);
			break;
        case CMD_GETHTTPHANDLEDPAGES:
            handle_CMD_GETHTTPHANDLEDPAGES(msg);
            break;
        case CMD_HTTPREQUEST:
            handle_CMD_HTTPREQUEST(msg);
            break;
	}			
	msg->freeData();
}


int WebESP8266::readByte(uint32_t timeOut)
{
	SoftTimeOut timeout(timeOut);
	while (!timeout)
		if (_stream->available() > 0)
			return _stream->read();
	return -1;
}


// note: timeout resets each time a byte is received
uint32_t WebESP8266::readBuffer(uint8_t* buffer, uint32_t size, uint32_t timeOut)
{
	for (uint32_t i = 0; i != size; ++i)
	{
		int b = readByte(timeOut);
		if (b < 0)
			return i;
		*buffer++ = b;
	}
	return size;
}


WebESP8266::Message WebESP8266::receive()
{
	Message msg;
	SoftTimeOut timeout(WAIT_MSG_TIMEOUT);
	while (!timeout)
	{
		// ID
		int16_t r = readByte(INTRA_MSG_TIMEOUT);
		if (r < 0)
			continue;
		msg.ID = r;

		// Command
		r = readByte(INTRA_MSG_TIMEOUT);
		if (r < 0)
			continue;
		msg.command = r;

		// Data Size Low
		r = readByte(INTRA_MSG_TIMEOUT);
		if (r < 0)
			continue;
		msg.dataSize = r;

		// Data Size High
		r = readByte(INTRA_MSG_TIMEOUT);
		if (r < 0)
			continue;
		msg.dataSize |= r << 8;

		// Data			
		if (msg.dataSize > 0 && msg.dataSize < MAX_DATA_SIZE)
		{
			msg.data = new uint8_t[msg.dataSize];
			if (readBuffer(msg.data, msg.dataSize, INTRA_MSG_TIMEOUT) < msg.dataSize)
			{
				msg.freeData();
				continue;
			}
		}
		
		// check ID
		if (msg.ID == _recvID)
		{
			msg.freeData();
			continue;
		}
		_recvID = msg.ID;
		
		msg.valid = true;
		return msg;
	}			
	return msg;
}


uint8_t WebESP8266::getNextID()
{
	return ++_sendID;
}


void WebESP8266::send(Message* msg)
{
	_stream->write(msg->ID);
	_stream->write(msg->command);
	_stream->write(msg->dataSize & 0xFF);
	_stream->write((msg->dataSize >> 8) & 0xFF);
	if (msg->dataSize > 0)
		_stream->write(msg->data, msg->dataSize);
}


// send ACK without parameters
void WebESP8266::sendNoParamsACK(uint8_t ackID)
{
	Message msgContainer(getNextID(), CMD_ACK, Message_ACK::SIZE);
	Message_ACK msgACK(&msgContainer, ackID);
	send(&msgContainer);
	msgContainer.freeData();			
}


WebESP8266::Message WebESP8266::waitACK(uint8_t ackID)
{
	Message msgContainer;
	SoftTimeOut timeout(GETACK_TIMEOUT);
	while (!timeout)
	{
		msgContainer = receive();
		if (msgContainer.valid)
		{
			Message_ACK msgACK(&msgContainer);
			if (msgACK.ackID == ackID)
				return msgContainer;
			msgContainer.freeData();	// discard this ACK
		}
	}
	msgContainer.valid = false;
	return msgContainer;
}


bool WebESP8266::waitNoParamsACK(uint8_t ackID)
{
	Message msgContainer = waitACK(ackID);
	if (msgContainer.valid)
	{
		msgContainer.freeData();
		return true;
	}
	return false;
}		


void WebESP8266::handle_CMD_READY(Message* msg)
{    
	// process message
	Message_CMD_READY msgCMDREADY(msg);
	_isReady  = (msgCMDREADY.protocolVersion == PROTOCOL_VERSION && strcmp_P(msgCMDREADY.magicString, STR_BINPRORDY) == 0);
	_platform = msgCMDREADY.platform;
	
	// send ACK with parameters
	Message msgContainer(getNextID(), CMD_ACK, Message_CMD_READY_ACK::SIZE);
	Message_CMD_READY_ACK msgCMDREADYACK(&msgContainer, msg->ID, PROTOCOL_VERSION, PLATFORM_THIS, STR_BINPRORDY);
	send(&msgContainer);
	msgContainer.freeData();
}


void WebESP8266::handle_CMD_IOCONF(Message* msg)
{
	// process message
	Message_CMD_IOCONF msgIOCONF(msg);
	if (msgIOCONF.flags & PIN_CONF_OUTPUT)
		pinMode(msgIOCONF.pin, OUTPUT);
	else
		pinMode(msgIOCONF.pin, (msgIOCONF.flags & PIN_CONF_PULLUP)? INPUT_PULLUP : INPUT);
				
	// send simple ACK
	sendNoParamsACK(msg->ID);
}


void WebESP8266::handle_CMD_IOSET(Message* msg)
{
	// process message
	Message_CMD_IOSET msgIOSET(msg);
	digitalWrite(msgIOSET.pin, msgIOSET.state);
	
	// send simple ACK
	sendNoParamsACK(msg->ID);
}


void WebESP8266::handle_CMD_IOGET(Message* msg)
{
	// process message
	Message_CMD_IOGET msgIOGET(msg);
	bool state = digitalRead(msgIOGET.pin);
	
	// send ACK with parameters
	Message msgContainer(getNextID(), CMD_ACK, Message_CMD_IOGET_ACK::SIZE);
	Message_CMD_IOGET_ACK msgCMDIOGETACK(&msgContainer, msg->ID, state);
	send(&msgContainer);
	msgContainer.freeData();
}


void WebESP8266::handle_CMD_IOASET(Message* msg)
{
	// process message
	Message_CMD_IOASET msgIOASET(msg);
	analogWrite(msgIOASET.pin, msgIOASET.state);
	
	// send simple ACK
	sendNoParamsACK(msg->ID);
}


void WebESP8266::handle_CMD_IOAGET(Message* msg)
{
	// process message
	Message_CMD_IOAGET msgIOAGET(msg);
	uint16_t state = analogRead(msgIOAGET.pin);
	
	// send ACK with parameters
	Message msgContainer(getNextID(), CMD_ACK, Message_CMD_IOAGET_ACK::SIZE);
	Message_CMD_IOAGET_ACK msgCMDIOGETACK(&msgContainer, msg->ID, state);
	send(&msgContainer);
	msgContainer.freeData();
}		


void WebESP8266::handle_CMD_GETHTTPHANDLEDPAGES(Message* msg)
{
    Serial.println("handle_CMD_GETHTTPHANDLEDPAGES");
    
    // calc message size
    uint16_t msgSize = sizeof(uint8_t);    // uint8_t for items count
    for (uint8_t i = 0; i != _webRoutesCount; ++i)
        msgSize += strlen_P(_webRoutes[i].page) + 1;
    
    // send ACK with parameters
    Message msgContainer(getNextID(), CMD_ACK, Message_CMD_IOAGET_ACK::SIZE + msgSize);
    Message_ACK msgACK(&msgContainer, msg->ID);
    uint8_t* wpos = msgContainer.data + Message_ACK::SIZE;
    *wpos++ = _webRoutesCount;
    for (uint8_t i = 0; i != _webRoutesCount; ++i)
    {        
        strcpy_P((char*)wpos, _webRoutes[i].page);
        wpos += strlen_P(_webRoutes[i].page) + 1;
    }
    send(&msgContainer);
    msgContainer.freeData();
}


void WebESP8266::handle_CMD_HTTPREQUEST(Message* msg)
{
    Serial.println("handle_CMD_HTTPREQUEST");
    
    //// decode message
    
    HTTPRequest request;
    
    uint8_t const* rpos = msg->data;
    
    // method (get, post...)
    request.method = (HTTPRequest::Method)*rpos++;
    
    // page index (as registered by handle_CMD_GETHTTPHANDLEDPAGES)
    request.pageIndex = *rpos++;
    
    // page (zero terminated string)
    request.page = (char const*)rpos;
    rpos += strlen(request.page) + 1;
    
    // headers count
    uint8_t headersCount = *rpos++;
    
    // headers key->value
    request.headers.reset((char const*)rpos, headersCount);
    rpos += request.headers.calcBufferSize();

    // query count
    uint8_t queryCount = *rpos++;
    
    // query key->value
    request.query.reset((char const*)rpos, queryCount);
    rpos += request.query.calcBufferSize();

    // form count
    uint8_t formCount = *rpos++;
    
    // form key->value
    request.form.reset((char const*)rpos, formCount);
    rpos += request.form.calcBufferSize();
    
    // call the handler and send ACK with parameters
    if (request.pageIndex < _webRoutesCount)
    {
        HTTPResponse response;
        _webRoutes[request.pageIndex].handler(request, response);
        uint16_t headersBufferSize = response.calcHeadersBufferSize();
        uint16_t contentBufferSize = response.calcContentBufferSize();
        uint16_t msgSize = 1 + 1 + 2 + headersBufferSize + contentBufferSize;   // +1 = status, +1 = headers fields count, +2 = content length
        Message msgContainer(getNextID(), CMD_ACK, Message_ACK::SIZE + msgSize);
        Message_ACK msgACK(&msgContainer, msg->ID);
        uint8_t* wpos = msgContainer.data + Message_ACK::SIZE;
        
        // status
        *wpos++ = response.getStatus();
        
        // header fields count
        *wpos++ = response.calcHeadersFieldsCount();
        
        // header fields
        wpos = response.copyHeadersToBuffer(wpos);
        
        // content length
        Serial.println(contentBufferSize);
        *wpos++ = contentBufferSize & 0xFF;
        *wpos++ = contentBufferSize >> 8;
        response.copyContentToBuffer(wpos);
        
        // send and free
        send(&msgContainer);
        msgContainer.freeData();        
    }    
    
}


bool WebESP8266::send_CMD_READY()
{
	_isReady = false;
	for (uint8_t i = 0; i != MAX_RESEND_COUNT; ++i)
	{
		// send message
		uint8_t msgID = getNextID();
		Message msgContainer(msgID, CMD_READY, Message_CMD_READY::SIZE);
		Message_CMD_READY msgCMDREADY(&msgContainer, PROTOCOL_VERSION, PLATFORM_THIS, STR_BINPRORDY);
		send(&msgContainer);
		msgContainer.freeData();
		
		// wait for ACK
		msgContainer = waitACK(msgID);
		if (msgContainer.valid)
		{
			Message_CMD_READY_ACK msgCMDREADYACK(&msgContainer);
			_isReady  = (msgCMDREADYACK.protocolVersion == PROTOCOL_VERSION && strcmp_P(msgCMDREADYACK.magicString, STR_BINPRORDY) == 0);
			_platform = msgCMDREADYACK.platform;
			msgContainer.freeData();
			return true;
		}
	}
	return false;
}


bool WebESP8266::send_CMD_IOCONF(uint8_t pin, uint8_t flags)
{
	if (checkReady())
	{
		for (uint32_t i = 0; i != MAX_RESEND_COUNT; ++i)
		{
			// send message
			uint8_t msgID = getNextID();
			Message msgContainer(msgID, CMD_IOCONF, Message_CMD_IOCONF::SIZE);
			Message_CMD_IOCONF msgCMDIOCONF(&msgContainer, pin, flags);
			send(&msgContainer);
			msgContainer.freeData();
			
			// wait for ACK
			if (waitNoParamsACK(msgID))
				return true;
		}
		_isReady = false;	// no more ready
	}
	return false;
}


bool WebESP8266::send_CMD_IOSET(uint8_t pin, uint8_t state)
{
	if (checkReady())
	{
		for (uint32_t i = 0; i != MAX_RESEND_COUNT; ++i)
		{
			// send message
			uint8_t msgID = getNextID();
			Message msgContainer(msgID, CMD_IOSET, Message_CMD_IOSET::SIZE);
			Message_CMD_IOSET msgCMDIOSET(&msgContainer, pin, state);
			send(&msgContainer);
			msgContainer.freeData();
			
			// wait for ACK
			if (waitNoParamsACK(msgID))
				return true;
		}
		_isReady = false;	// no more ready
	}
	return false;
}


bool WebESP8266::send_CMD_IOGET(uint8_t pin, uint8_t* state)
{
	if (checkReady())
	{
		for (uint32_t i = 0; i != MAX_RESEND_COUNT; ++i)
		{
			// send message
			uint8_t msgID = getNextID();
			Message msgContainer(msgID, CMD_IOGET, Message_CMD_IOGET::SIZE);
			Message_CMD_IOGET msgCMDIOGET(&msgContainer, pin);
			send(&msgContainer);
			msgContainer.freeData();
			
			// wait for ACK
			msgContainer = waitACK(msgID);
			if (msgContainer.valid)
			{
				Message_CMD_IOGET_ACK msgCMDIOGETACK(&msgContainer);
				*state = msgCMDIOGETACK.state;
				msgContainer.freeData();
				return true;
			}
		}
		_isReady = false;	// no more ready
	}
	return false;
}


bool WebESP8266::send_CMD_IOASET(uint8_t pin, uint16_t state)
{
	if (checkReady())
	{
		for (uint32_t i = 0; i != MAX_RESEND_COUNT; ++i)
		{
			// send message
			uint8_t msgID = getNextID();
			Message msgContainer(msgID, CMD_IOASET, Message_CMD_IOASET::SIZE);
			Message_CMD_IOASET msgCMDIOASET(&msgContainer, pin, state);
			send(&msgContainer);
			msgContainer.freeData();
			
			// wait for ACK
			if (waitNoParamsACK(msgID))
				return true;
		}
		_isReady = false;	// no more ready
	}
	return false;
}


bool WebESP8266::send_CMD_IOAGET(uint8_t pin, uint16_t* state)
{
	if (checkReady())
	{
		for (uint32_t i = 0; i != MAX_RESEND_COUNT; ++i)
		{
			// send message
			uint8_t msgID = getNextID();
			Message msgContainer(msgID, CMD_IOAGET, Message_CMD_IOAGET::SIZE);
			Message_CMD_IOAGET msgCMDIOAGET(&msgContainer, pin);
			send(&msgContainer);
			msgContainer.freeData();
			
			// wait for ACK
			msgContainer = waitACK(msgID);
			if (msgContainer.valid)
			{
				Message_CMD_IOAGET_ACK msgCMDIOAGETACK(&msgContainer);
				*state = msgCMDIOAGETACK.state;
				msgContainer.freeData();
				return true;
			}
		}
		_isReady = false;	// no more ready
	}
	return false;
}


