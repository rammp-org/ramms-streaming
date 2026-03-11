// Copyright Epic Games, Inc. All Rights Reserved.

#include "RammsStreamProtocol.h"
#include "Serialization/BufferArchive.h"

// ---------------------------------------------------------------------------
// FRammsStreamHeader
// ---------------------------------------------------------------------------

void FRammsStreamHeader::ToBytes(uint8* Out) const
{
	// Magic
	Out[0] = MAGIC[0];
	Out[1] = MAGIC[1];
	Out[2] = MAGIC[2];
	Out[3] = MAGIC[3];
	// Version
	Out[4] = VERSION;
	// MessageType
	Out[5] = static_cast<uint8>(MessageType);
	// ChannelID (LE)
	Out[6] = ChannelID & 0xFF;
	Out[7] = (ChannelID >> 8) & 0xFF;
	// Flags (LE)
	Out[8] = Flags & 0xFF;
	Out[9] = (Flags >> 8) & 0xFF;
	// SequenceNum (LE)
	Out[10] = SequenceNum & 0xFF;
	Out[11] = (SequenceNum >> 8) & 0xFF;
	Out[12] = (SequenceNum >> 16) & 0xFF;
	Out[13] = (SequenceNum >> 24) & 0xFF;
	// Timestamp (LE, int64)
	for (int i = 0; i < 8; ++i)
	{
		Out[14 + i] = (Timestamp >> (i * 8)) & 0xFF;
	}
	// MetadataLen (LE)
	Out[22] = MetadataLen & 0xFF;
	Out[23] = (MetadataLen >> 8) & 0xFF;
	Out[24] = (MetadataLen >> 16) & 0xFF;
	Out[25] = (MetadataLen >> 24) & 0xFF;
	// PayloadLen (LE)
	Out[26] = PayloadLen & 0xFF;
	Out[27] = (PayloadLen >> 8) & 0xFF;
	Out[28] = (PayloadLen >> 16) & 0xFF;
	Out[29] = (PayloadLen >> 24) & 0xFF;
	// Reserved
	Out[30] = 0;
	Out[31] = 0;
}

bool FRammsStreamHeader::FromBytes(const uint8* In)
{
	if (!ValidateHeader(In))
		return false;

	MessageType = static_cast<ERammsStreamMessageType>(In[5]);
	ChannelID = In[6] | (static_cast<uint16>(In[7]) << 8);
	Flags = In[8] | (static_cast<uint16>(In[9]) << 8);
	SequenceNum = In[10] | (static_cast<uint32>(In[11]) << 8) | (static_cast<uint32>(In[12]) << 16) | (static_cast<uint32>(In[13]) << 24);

	Timestamp = 0;
	for (int i = 0; i < 8; ++i)
	{
		Timestamp |= static_cast<int64>(In[14 + i]) << (i * 8);
	}

	MetadataLen = In[22] | (static_cast<uint32>(In[23]) << 8) | (static_cast<uint32>(In[24]) << 16) | (static_cast<uint32>(In[25]) << 24);
	PayloadLen = In[26] | (static_cast<uint32>(In[27]) << 8) | (static_cast<uint32>(In[28]) << 16) | (static_cast<uint32>(In[29]) << 24);

	return true;
}

bool FRammsStreamHeader::ValidateHeader(const uint8* Buffer)
{
	return Buffer[0] == MAGIC[0] && Buffer[1] == MAGIC[1] && Buffer[2] == MAGIC[2] && Buffer[3] == MAGIC[3] && Buffer[4] == VERSION;
}

// ---------------------------------------------------------------------------
// FRammsStreamMessage
// ---------------------------------------------------------------------------

void FRammsStreamMessage::SetMetadataString(const FString& JsonString)
{
	FTCHARToUTF8 Utf8(*JsonString);
	Metadata.SetNumUninitialized(Utf8.Length());
	FMemory::Memcpy(Metadata.GetData(), Utf8.Get(), Utf8.Length());
}

FString FRammsStreamMessage::GetMetadataString() const
{
	if (Metadata.Num() == 0)
		return FString();
	FUTF8ToTCHAR Conv(reinterpret_cast<const ANSICHAR*>(Metadata.GetData()),
		Metadata.Num());
	return FString(Conv.Length(), Conv.Get());
}

TArray<uint8> FRammsStreamMessage::Serialize() const
{
	// Build a mutable copy of the header with correct lengths
	FRammsStreamHeader H = Header;
	H.MetadataLen = static_cast<uint32>(Metadata.Num());
	H.PayloadLen = static_cast<uint32>(Payload.Num());

	TArray<uint8> Buffer;
	Buffer.SetNumUninitialized(FRammsStreamHeader::HEADER_SIZE + H.MetadataLen + H.PayloadLen);

	H.ToBytes(Buffer.GetData());

	if (H.MetadataLen > 0)
	{
		FMemory::Memcpy(Buffer.GetData() + FRammsStreamHeader::HEADER_SIZE,
			Metadata.GetData(), H.MetadataLen);
	}
	if (H.PayloadLen > 0)
	{
		FMemory::Memcpy(Buffer.GetData() + FRammsStreamHeader::HEADER_SIZE + H.MetadataLen,
			Payload.GetData(), H.PayloadLen);
	}

	return Buffer;
}

bool FRammsStreamMessage::Deserialize(const uint8* InBuffer, int32 BufferLen,
	FRammsStreamMessage& OutMsg,
	int32&				 OutBytesConsumed)
{
	OutBytesConsumed = 0;

	if (BufferLen < static_cast<int32>(FRammsStreamHeader::HEADER_SIZE))
	{
		return false; // need more data
	}

	FRammsStreamHeader H;
	if (!H.FromBytes(InBuffer))
	{
		return false; // bad header
	}

	const int32 TotalSize = static_cast<int32>(H.TotalMessageSize());
	if (BufferLen < TotalSize)
	{
		return false; // need more data
	}

	OutMsg.Header = H;

	const uint8* MetaStart = InBuffer + FRammsStreamHeader::HEADER_SIZE;
	if (H.MetadataLen > 0)
	{
		OutMsg.Metadata.SetNumUninitialized(H.MetadataLen);
		FMemory::Memcpy(OutMsg.Metadata.GetData(), MetaStart, H.MetadataLen);
	}
	else
	{
		OutMsg.Metadata.Empty();
	}

	const uint8* PayloadStart = MetaStart + H.MetadataLen;
	if (H.PayloadLen > 0)
	{
		OutMsg.Payload.SetNumUninitialized(H.PayloadLen);
		FMemory::Memcpy(OutMsg.Payload.GetData(), PayloadStart, H.PayloadLen);
	}
	else
	{
		OutMsg.Payload.Empty();
	}

	OutBytesConsumed = TotalSize;
	return true;
}
