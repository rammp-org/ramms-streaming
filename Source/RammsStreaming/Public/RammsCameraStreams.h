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
 * These are the stream ids, roles and display names ramms-tools' StreamSender
 * already sends toward UE, so a client reading them -- rammp-ui's camera
 * bridge, ramms-tools -- can associate the four streams of one camera by its
 * "group" without knowing the channel layout.
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
	 * the association keys stream_id, group, role, name and fmt. Motion and mask
	 * frames describe their own payload: "w"/"h" become the motion grid (plus
	 * motion_w/motion_h) or the class-mask grid. Mask frames add
	 * material_params and MaterialScalarParameters ({"NumSegmentIDs": highest
	 * class value + 1}; the second key is the one rammp-ui's camera bridge and
	 * ramms-tools' StreamSender use on the wire) and classes
	 * ({"<value>": "<fine class>"}) from SegTable. Without a table the mask's
	 * NumSegmentIDs is its highest pixel value + 1 and classes is empty.
	 */
	static FString BuildFrameMetadataJson(const FCaptureData& Data, const FString& StreamName, ERammsCameraStreamRole Role,
		const FCameraCaptureSegTable* SegTable = nullptr);

	/** The keys the source sent before streams carried an association: the RGBD path still sends exactly these. */
	static TSharedRef<FJsonObject> BuildBaseMetadata(const FCaptureData& Data);

	/** Highest fine class value in the table + 1 (1 when only "unlabeled" is defined). */
	static int32 NumSegmentIds(const FCameraCaptureSegTable& SegTable);
};
