// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "RammsStreamProtocol.generated.h"

/** Message types for the RMSS binary protocol. */
UENUM(BlueprintType)
enum class ERammsStreamMessageType : uint8
{
	None = 0x00,

	// Camera / sensor data (UE → external)
	FrameRGB = 0x01,	// BGRA8 pixel data
	FrameDepth = 0x02,	// float32 per pixel (cm)
	FrameRGBD = 0x03,	// BGRA8 followed by float32 depth
	FrameMotion = 0x04, // float32×2 motion vectors

	// Future spatial data
	PointCloud = 0x05,
	OctoMap = 0x06,

	// Generic image (external → UE)
	ImageData = 0x10,

	// Generic frame data (bidirectional, pixel format described by "fmt" metadata)
	FrameData = 0x11,

	// Control messages
	MetadataOnly = 0xF0,
	Subscribe = 0xF1,
	Unsubscribe = 0xF2,
	Ack = 0xFD,
	Error = 0xFE,
	Ping = 0xFF,
};

/** High-level classification of a message type. */
UENUM(BlueprintType)
enum class ERammsFrameCategory : uint8
{
	/** Renderable image data (RGB, RGBD, depth visualisation, masks). */
	Visual,
	/** Non-renderable auxiliary data (motion vectors, point clouds, etc.). */
	Data,
	/** Control / metadata-only messages. */
	Control,
};

/** Classify a message type into a frame category. */
inline ERammsFrameCategory GetFrameCategory(ERammsStreamMessageType Type)
{
	switch (Type)
	{
		case ERammsStreamMessageType::FrameRGB:
		case ERammsStreamMessageType::FrameDepth:
		case ERammsStreamMessageType::FrameRGBD:
		case ERammsStreamMessageType::ImageData:
			return ERammsFrameCategory::Visual;

		case ERammsStreamMessageType::FrameMotion:
		case ERammsStreamMessageType::PointCloud:
		case ERammsStreamMessageType::OctoMap:
		case ERammsStreamMessageType::FrameData:
			return ERammsFrameCategory::Data;

		default:
			return ERammsFrameCategory::Control;
	}
}

/** Compression mode stored in the Flags field. */
UENUM(BlueprintType)
enum class ERammsStreamCompression : uint8
{
	None = 0,
	LZ4 = 1,
	JPEG = 2,
	PNG = 3,
};

/**
 * 32-byte on-wire header for every RMSS message.
 *
 * Layout (all multi-byte fields are little-endian):
 *   [0..3]   Magic          "RMSS"
 *   [4]      Version        1
 *   [5]      MessageType    ERammsStreamMessageType
 *   [6..7]   ChannelID      uint16 camera/stream identifier
 *   [8..9]   Flags          uint16 bitfield (compression, priority, etc.)
 *   [10..13] SequenceNum    uint32 monotonic counter
 *   [14..21] Timestamp      int64  microseconds since epoch
 *   [22..25] MetadataLen    uint32 JSON metadata byte count
 *   [26..29] PayloadLen     uint32 binary payload byte count
 *   [30..31] Reserved       2 bytes (zero)
 */
USTRUCT(BlueprintType)
struct RAMMSSTREAMING_API FRammsStreamHeader
{
	GENERATED_BODY()

	static constexpr uint32 HEADER_SIZE = 32;
	static constexpr uint8	MAGIC[4] = { 'R', 'M', 'S', 'S' };
	static constexpr uint8	VERSION = 1;

	// Flag bit positions
	static constexpr uint16 FLAG_COMPRESSED = 0x0001;
	static constexpr uint16 FLAG_COMP_TYPE_MASK = 0x0006; // bits 1-2
	static constexpr uint16 FLAG_COMP_TYPE_SHIFT = 1;
	static constexpr uint16 FLAG_HAS_ALPHA = 0x0008;
	static constexpr uint16 FLAG_HIGH_PRIORITY = 0x0010;

	UPROPERTY()
	ERammsStreamMessageType MessageType = ERammsStreamMessageType::None;

	UPROPERTY()
	uint16 ChannelID = 0;

	UPROPERTY()
	uint16 Flags = 0;

	UPROPERTY()
	uint32 SequenceNum = 0;

	UPROPERTY()
	int64 Timestamp = 0; // microseconds since epoch

	UPROPERTY()
	uint32 MetadataLen = 0;

	UPROPERTY()
	uint32 PayloadLen = 0;

	/** Total message size including header. */
	uint32 TotalMessageSize() const
	{
		return HEADER_SIZE + MetadataLen + PayloadLen;
	}

	/** Set compression type in flags. */
	void SetCompression(ERammsStreamCompression Comp)
	{
		Flags &= ~(FLAG_COMPRESSED | FLAG_COMP_TYPE_MASK);
		if (Comp != ERammsStreamCompression::None)
		{
			Flags |= FLAG_COMPRESSED;
			Flags |= (static_cast<uint16>(Comp) << FLAG_COMP_TYPE_SHIFT) & FLAG_COMP_TYPE_MASK;
		}
	}

	/** Get compression type from flags. */
	ERammsStreamCompression GetCompression() const
	{
		if (!(Flags & FLAG_COMPRESSED))
			return ERammsStreamCompression::None;
		return static_cast<ERammsStreamCompression>((Flags & FLAG_COMP_TYPE_MASK) >> FLAG_COMP_TYPE_SHIFT);
	}

	/** Serialize this header into a 32-byte buffer (little-endian). */
	void ToBytes(uint8* OutBuffer) const;

	/** Deserialize a 32-byte buffer into this header. Returns false if
	 * magic/version mismatch. */
	bool FromBytes(const uint8* InBuffer);

	/** Validate magic bytes and version. */
	static bool ValidateHeader(const uint8* Buffer);
};

/**
 * Complete RMSS message: header + optional metadata JSON + optional binary
 * payload.
 */
USTRUCT()
struct RAMMSSTREAMING_API FRammsStreamMessage
{
	GENERATED_BODY()

	FRammsStreamHeader Header;

	/** JSON metadata (UTF-8 encoded). */
	TArray<uint8> Metadata;

	/** Binary payload (pixel data, compressed frames, etc.). */
	TArray<uint8> Payload;

	/** Convenience: set metadata from a JSON string. */
	void SetMetadataString(const FString& JsonString);

	/** Convenience: get metadata as a JSON string. */
	FString GetMetadataString() const;

	/** Serialize entire message (header + metadata + payload) into a contiguous
	 * buffer. */
	TArray<uint8> Serialize() const;

	/**
	 * Attempt to parse a message from a raw byte buffer.
	 * @param InBuffer   Pointer to the beginning of the data.
	 * @param BufferLen  Number of available bytes.
	 * @param OutBytesConsumed  Set to number of bytes consumed on success.
	 * @return true if a complete message was parsed.
	 */
	static bool Deserialize(const uint8* InBuffer, int32 BufferLen,
		FRammsStreamMessage& OutMsg, int32& OutBytesConsumed);
};
