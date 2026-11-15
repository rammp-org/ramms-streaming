// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "RammsStreamProtocol.h"

class FJsonObject;
struct FCaptureData;
struct FCameraCaptureSegTable;

/**
 * The streams one camera can produce, each on its own channel offset from the
 * source's base channel C.
 *
 * | role   | message           | channel | stream_id             | fmt     |
 * |--------|-------------------|---------|-----------------------|---------|
 * | color  | FrameRGB  0x01    | C       | camera/<name>/color   | bgra8   |
 * | depth  | FrameDepth 0x02   | C+100   | camera/<name>/depth   | float32 |
 * | motion | FrameMotion 0x04  | C+200   | motion/<name>         | rg32f   |
 * | mask   | FrameData 0x11    | C+300   | mask/<name>           | mono8   |
 *
 * The stream ids, roles and groups match the conventions ramms-tools and
 * rammp-ui use, so a client can put the four streams of one camera together by
 * "group" without knowing the channel layout. The message types are UE's own:
 * colour goes out as FrameRGB, which RammsStreaming's sink component does not
 * turn into a texture (it takes colour as ImageData 0x10). Names are not unique
 * across robots: every frame carries "actor" (the camera owner's object path),
 * and a client picks a robot's camera by actor (and channel), never by name
 * alone.
 */
enum class ERammsCameraStreamRole : uint8
{
	Color,
	Depth,
	Motion,
	Mask,
};

/** One stream a source sends: what a client subscribes to and how to read it. */
struct RAMMSSTREAMING_API FRammsCameraStreamDesc
{
	ERammsCameraStreamRole Role = ERammsCameraStreamRole::Color;
	/** The stream name ("group"): the camera's name. */
	FString					Name;
	FString					StreamId;
	int32					Channel = 0;
	ERammsStreamMessageType Message = ERammsStreamMessageType::FrameRGB;
	FString					Format;
};

/** One role of one captured frame, ready to send: the payload in the role's wire format and its metadata. */
struct RAMMSSTREAMING_API FRammsCameraFrameOut
{
	ERammsCameraStreamRole	Role = ERammsCameraStreamRole::Color;
	int32					Channel = 0;
	ERammsStreamMessageType Message = ERammsStreamMessageType::FrameRGB;
	/** The payload's grid: the colour, depth, motion or class-mask grid. */
	int32		  Width = 0;
	int32		  Height = 0;
	TArray<uint8> Payload;
	FString		  MetadataJson;
	/** Which stream of its channel this is (URammsStreamingSubsystem::MakeStreamKey of its stream id, actor and
	 *  camera), so the frames of several cameras a source sends on one channel do not take each other's place. */
	uint64 StreamKey = 0;
};

/**
 * The stream conventions above as plain functions, shared by the stream source
 * (which sends) and anything that describes what is sent (RammsCore's sensor
 * rig and bridge). Pure: no world, no subsystem.
 */
struct RAMMSSTREAMING_API FRammsCameraStreams
{
	/** Channel offsets from the base channel. Depth's +100 predates the rest. */
	static constexpr int32 DepthChannelOffset = 100;
	static constexpr int32 MotionChannelOffset = 200;
	static constexpr int32 MaskChannelOffset = 300;

	/** Highest channel offset; a base channel above 65535 minus this does not fit a uint16 header. */
	static constexpr int32 MaxChannelOffset = MaskChannelOffset;

	/** Highest base channel whose four channels all fit the header's uint16. */
	static constexpr int32 MaxBaseChannel = MAX_uint16 - MaxChannelOffset;

	/** Whether all four channels of base channel C fit the header: 0 <= C <= MaxBaseChannel. */
	static bool IsValidBaseChannel(int32 BaseChannel) { return BaseChannel >= 0 && BaseChannel <= MaxBaseChannel; }

	static const TArray<ERammsCameraStreamRole>& AllRoles();

	static int32 ChannelOffset(ERammsCameraStreamRole Role);

	/** "color", "depth", "motion", "mask". */
	static FString RoleName(ERammsCameraStreamRole Role);

	/** "bgra8", "float32", "rg32f", "mono8". */
	static FString Format(ERammsCameraStreamRole Role);

	static ERammsStreamMessageType MessageType(ERammsCameraStreamRole Role);

	/** "FrameRGB", "FrameDepth", "FrameMotion", "FrameData". */
	static FString MessageName(ERammsCameraStreamRole Role);

	/** "camera/<name>/color", "camera/<name>/depth", "motion/<name>", "mask/<name>". */
	static FString StreamId(const FString& StreamName, ERammsCameraStreamRole Role);

	/** "<name> Color", "<name> Depth", "<name> Motion", "<name> Mask". */
	static FString DisplayName(const FString& StreamName, ERammsCameraStreamRole Role);

	/** A full description of one stream of a camera whose base channel is BaseChannel. */
	static FRammsCameraStreamDesc Describe(const FString& StreamName, ERammsCameraStreamRole Role, int32 BaseChannel);

	/**
	 * The metadata JSON a source sends with one frame of one role.
	 *
	 * Every role keeps the keys the source has always sent (w, h, depth_w,
	 * depth_h, frame, timestamp, camera, actor, transform, intrinsics) and adds
	 * the association keys stream_id, group, role, name and fmt. "actor" is the
	 * camera owner's object path. "w"/"h" describe the payload: the colour grid,
	 * the depth grid (with "unit": "cm"), the motion grid (with motion_w /
	 * motion_h and "unit": "px/frame") or the class-mask grid. Mask frames add
	 * material_params and MaterialScalarParameters ({"NumSegmentIDs": highest
	 * value the mask can hold + 1, see NumSegmentIds; the second key is the one
	 * rammp-ui's camera bridge and ramms-tools' StreamSender use), classes
	 * ({"<value>": "<fine class>"}) and reserved (the reserved stencil values,
	 * which can appear in the mask unnamed) from SegTable. Without a table the
	 * mask's NumSegmentIDs is its highest pixel value + 1 and classes and
	 * reserved are empty.
	 */
	static FString BuildFrameMetadataJson(const FCaptureData& Data, const FString& StreamName, ERammsCameraStreamRole Role,
		const FCameraCaptureSegTable* SegTable = nullptr);

	/** The keys the source sent before streams carried an association: the RGBD path still sends exactly these. */
	static TSharedRef<FJsonObject> BuildBaseMetadata(const FCaptureData& Data);

	/** The highest value a mask can hold + 1: the highest fine class value in the table or reserved stencil value
	 *  (something else's custom-depth stencil, kept as it is and so present in the mask), whichever is higher; 1
	 *  when only "unlabeled" is defined. */
	static int32 NumSegmentIds(const FCameraCaptureSegTable& SegTable);

	/** The stream key of one role of one camera's frames (URammsStreamingSubsystem::MakeStreamKey of its stream id,
	 *  the camera's actor and its capture id). */
	static uint64 StreamKey(const FCaptureData& Data, const FString& StreamName, ERammsCameraStreamRole Role);

	/** Whether Data carries the plane Role sends: colour pixels, depth, a full motion grid, a full class mask. */
	static bool HasRole(const FCaptureData& Data, ERammsCameraStreamRole Role);

	/**
	 * One role of a captured frame as the message a source sends for it on BaseChannel's channel for that role:
	 * the payload in the role's wire format (bgra8 colour, float32 depth in cm, rg32f motion from the capture's
	 * double-precision vectors, mono8 classes) and BuildFrameMetadataJson's metadata. False when Data has no such
	 * plane (HasRole) or the role's channel does not fit the header (0..65535).
	 */
	static bool BuildFrame(const FCaptureData& Data, const FString& StreamName, ERammsCameraStreamRole Role,
		int32 BaseChannel, const FCameraCaptureSegTable* SegTable, FRammsCameraFrameOut& OutFrame);
};
