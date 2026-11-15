// Copyright Epic Games, Inc. All Rights Reserved.

#include "RammsCameraStreams.h"
#include "CameraCaptureSegmentation.h"
#include "CameraCaptureSubsystem.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

const TArray<ERammsCameraStreamRole>& FRammsCameraStreams::AllRoles()
{
	static const TArray<ERammsCameraStreamRole> Roles = {
		ERammsCameraStreamRole::Color,
		ERammsCameraStreamRole::Depth,
		ERammsCameraStreamRole::Motion,
		ERammsCameraStreamRole::Mask,
	};
	return Roles;
}

int32 FRammsCameraStreams::ChannelOffset(ERammsCameraStreamRole Role)
{
	switch (Role)
	{
		case ERammsCameraStreamRole::Depth:
			return DepthChannelOffset;
		case ERammsCameraStreamRole::Motion:
			return MotionChannelOffset;
		case ERammsCameraStreamRole::Mask:
			return MaskChannelOffset;
		case ERammsCameraStreamRole::Color:
		default:
			return 0;
	}
}

FString FRammsCameraStreams::RoleName(ERammsCameraStreamRole Role)
{
	switch (Role)
	{
		case ERammsCameraStreamRole::Depth:
			return TEXT("depth");
		case ERammsCameraStreamRole::Motion:
			return TEXT("motion");
		case ERammsCameraStreamRole::Mask:
			return TEXT("mask");
		case ERammsCameraStreamRole::Color:
		default:
			return TEXT("color");
	}
}

FString FRammsCameraStreams::Format(ERammsCameraStreamRole Role)
{
	switch (Role)
	{
		case ERammsCameraStreamRole::Depth:
			return TEXT("float32");
		case ERammsCameraStreamRole::Motion:
			return TEXT("rg32f");
		case ERammsCameraStreamRole::Mask:
			return TEXT("mono8");
		case ERammsCameraStreamRole::Color:
		default:
			return TEXT("bgra8");
	}
}

ERammsStreamMessageType FRammsCameraStreams::MessageType(ERammsCameraStreamRole Role)
{
	switch (Role)
	{
		case ERammsCameraStreamRole::Depth:
			return ERammsStreamMessageType::FrameDepth;
		case ERammsCameraStreamRole::Motion:
			return ERammsStreamMessageType::FrameMotion;
		case ERammsCameraStreamRole::Mask:
			return ERammsStreamMessageType::FrameData;
		case ERammsCameraStreamRole::Color:
		default:
			return ERammsStreamMessageType::FrameRGB;
	}
}

FString FRammsCameraStreams::MessageName(ERammsCameraStreamRole Role)
{
	switch (Role)
	{
		case ERammsCameraStreamRole::Depth:
			return TEXT("FrameDepth");
		case ERammsCameraStreamRole::Motion:
			return TEXT("FrameMotion");
		case ERammsCameraStreamRole::Mask:
			return TEXT("FrameData");
		case ERammsCameraStreamRole::Color:
		default:
			return TEXT("FrameRGB");
	}
}

FString FRammsCameraStreams::StreamId(const FString& StreamName, ERammsCameraStreamRole Role)
{
	switch (Role)
	{
		case ERammsCameraStreamRole::Depth:
			return FString::Printf(TEXT("camera/%s/depth"), *StreamName);
		case ERammsCameraStreamRole::Motion:
			return FString::Printf(TEXT("motion/%s"), *StreamName);
		case ERammsCameraStreamRole::Mask:
			return FString::Printf(TEXT("mask/%s"), *StreamName);
		case ERammsCameraStreamRole::Color:
		default:
			return FString::Printf(TEXT("camera/%s/color"), *StreamName);
	}
}

FString FRammsCameraStreams::DisplayName(const FString& StreamName, ERammsCameraStreamRole Role)
{
	switch (Role)
	{
		case ERammsCameraStreamRole::Depth:
			return StreamName + TEXT(" Depth");
		case ERammsCameraStreamRole::Motion:
			return StreamName + TEXT(" Motion");
		case ERammsCameraStreamRole::Mask:
			return StreamName + TEXT(" Mask");
		case ERammsCameraStreamRole::Color:
		default:
			return StreamName + TEXT(" Color");
	}
}

FRammsCameraStreamDesc FRammsCameraStreams::Describe(const FString& StreamName, ERammsCameraStreamRole Role, int32 BaseChannel)
{
	FRammsCameraStreamDesc Desc;
	Desc.Role = Role;
	Desc.Name = StreamName;
	Desc.StreamId = StreamId(StreamName, Role);
	Desc.Channel = BaseChannel + ChannelOffset(Role);
	Desc.Message = MessageType(Role);
	Desc.Format = Format(Role);
	return Desc;
}

TSharedRef<FJsonObject> FRammsCameraStreams::BuildBaseMetadata(const FCaptureData& Data)
{
	TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();

	Json->SetNumberField(TEXT("w"), Data.Width);
	Json->SetNumberField(TEXT("h"), Data.Height);
	if (Data.DepthWidth > 0 && Data.DepthHeight > 0)
	{
		Json->SetNumberField(TEXT("depth_w"), Data.DepthWidth);
		Json->SetNumberField(TEXT("depth_h"), Data.DepthHeight);
	}
	Json->SetNumberField(TEXT("frame"), Data.FrameNumber);
	Json->SetNumberField(TEXT("timestamp"), Data.Timestamp);
	Json->SetStringField(TEXT("camera"), Data.CameraID.ToString());
	Json->SetStringField(TEXT("actor"), Data.ActorPath);

	// Transform
	const FVector			Loc = Data.WorldTransform.GetLocation();
	const FRotator			Rot = Data.WorldTransform.Rotator();
	TSharedRef<FJsonObject> TransformJson = MakeShared<FJsonObject>();
	TransformJson->SetNumberField(TEXT("x"), Loc.X);
	TransformJson->SetNumberField(TEXT("y"), Loc.Y);
	TransformJson->SetNumberField(TEXT("z"), Loc.Z);
	TransformJson->SetNumberField(TEXT("pitch"), Rot.Pitch);
	TransformJson->SetNumberField(TEXT("yaw"), Rot.Yaw);
	TransformJson->SetNumberField(TEXT("roll"), Rot.Roll);
	Json->SetObjectField(TEXT("transform"), TransformJson);

	// Intrinsics
	TSharedRef<FJsonObject> IntrinsicsJson = MakeShared<FJsonObject>();
	IntrinsicsJson->SetNumberField(TEXT("fx"), Data.Intrinsics.FocalLengthX);
	IntrinsicsJson->SetNumberField(TEXT("fy"), Data.Intrinsics.FocalLengthY);
	IntrinsicsJson->SetNumberField(TEXT("cx"), Data.Intrinsics.PrincipalPointX);
	IntrinsicsJson->SetNumberField(TEXT("cy"), Data.Intrinsics.PrincipalPointY);
	IntrinsicsJson->SetNumberField(TEXT("res_x"), Data.Intrinsics.ImageWidth);
	IntrinsicsJson->SetNumberField(TEXT("res_y"), Data.Intrinsics.ImageHeight);
	Json->SetObjectField(TEXT("intrinsics"), IntrinsicsJson);

	return Json;
}

int32 FRammsCameraStreams::NumSegmentIds(const FCameraCaptureSegTable& SegTable)
{
	int32 Highest = 0;
	for (int32 Value = 0; Value < SegTable.FineNames.Num(); ++Value)
	{
		if (!SegTable.FineNames[Value].IsNone())
		{
			Highest = Value;
		}
	}
	return Highest + 1;
}

FString FRammsCameraStreams::BuildFrameMetadataJson(const FCaptureData& Data, const FString& StreamName, ERammsCameraStreamRole Role,
	const FCameraCaptureSegTable* SegTable)
{
	TSharedRef<FJsonObject> Json = BuildBaseMetadata(Data);

	// Association: what lets a client put the four streams of one camera together.
	Json->SetStringField(TEXT("stream_id"), StreamId(StreamName, Role));
	Json->SetStringField(TEXT("group"), StreamName);
	Json->SetStringField(TEXT("role"), RoleName(Role));
	Json->SetStringField(TEXT("name"), DisplayName(StreamName, Role));
	Json->SetStringField(TEXT("fmt"), Format(Role));

	if (Role == ERammsCameraStreamRole::Depth)
	{
		// The payload is the depth grid, which need not be the colour grid.
		const int32 DepthW = Data.DepthWidth > 0 ? Data.DepthWidth : Data.Width;
		const int32 DepthH = Data.DepthHeight > 0 ? Data.DepthHeight : Data.Height;
		Json->SetNumberField(TEXT("w"), DepthW);
		Json->SetNumberField(TEXT("h"), DepthH);
		Json->SetNumberField(TEXT("depth_w"), DepthW);
		Json->SetNumberField(TEXT("depth_h"), DepthH);
		Json->SetStringField(TEXT("unit"), TEXT("cm"));
	}
	else if (Role == ERammsCameraStreamRole::Motion)
	{
		// The payload is the motion grid, which is its own pass and need not be
		// the colour grid; "w"/"h" describe the payload, as they do for depth.
		Json->SetNumberField(TEXT("w"), Data.MotionWidth);
		Json->SetNumberField(TEXT("h"), Data.MotionHeight);
		Json->SetNumberField(TEXT("motion_w"), Data.MotionWidth);
		Json->SetNumberField(TEXT("motion_h"), Data.MotionHeight);
		Json->SetStringField(TEXT("unit"), TEXT("px/frame"));
	}
	else if (Role == ERammsCameraStreamRole::Mask)
	{
		Json->SetNumberField(TEXT("w"), Data.ClassWidth);
		Json->SetNumberField(TEXT("h"), Data.ClassHeight);

		int32					NumIds = 1;
		TSharedRef<FJsonObject> Classes = MakeShared<FJsonObject>();
		if (SegTable)
		{
			NumIds = NumSegmentIds(*SegTable);
			for (int32 Value = 0; Value < SegTable->FineNames.Num(); ++Value)
			{
				if (!SegTable->FineNames[Value].IsNone())
				{
					Classes->SetStringField(FString::FromInt(Value), SegTable->FineNames[Value].ToString());
				}
			}
		}
		else
		{
			uint8 HighestPixel = 0;
			for (const uint8 Pixel : Data.ClassData)
			{
				HighestPixel = FMath::Max(HighestPixel, Pixel);
			}
			NumIds = static_cast<int32>(HighestPixel) + 1;
		}

		TSharedRef<FJsonObject> MaterialParams = MakeShared<FJsonObject>();
		MaterialParams->SetNumberField(TEXT("NumSegmentIDs"), NumIds);
		Json->SetObjectField(TEXT("material_params"), MaterialParams);
		Json->SetObjectField(TEXT("MaterialScalarParameters"), MaterialParams);
		Json->SetObjectField(TEXT("classes"), Classes);
	}

	FString															 OutputString;
	TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
		TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&OutputString);
	FJsonSerializer::Serialize(Json, Writer);
	return OutputString;
}

bool FRammsCameraStreams::HasRole(const FCaptureData& Data, ERammsCameraStreamRole Role)
{
	switch (Role)
	{
		case ERammsCameraStreamRole::Depth:
			return Data.DepthData.Num() > 0;
		case ERammsCameraStreamRole::Motion:
			return Data.MotionWidth > 0 && Data.MotionHeight > 0
				&& Data.MotionVectorData.Num() == Data.MotionWidth * Data.MotionHeight;
		case ERammsCameraStreamRole::Mask:
			return Data.ClassWidth > 0 && Data.ClassHeight > 0 && Data.ClassData.Num() == Data.ClassWidth * Data.ClassHeight;
		case ERammsCameraStreamRole::Color:
		default:
			return Data.ImageData.Num() > 0;
	}
}

bool FRammsCameraStreams::BuildFrame(const FCaptureData& Data, const FString& StreamName, ERammsCameraStreamRole Role,
	int32 BaseChannel, const FCameraCaptureSegTable* SegTable, FRammsCameraFrameOut& OutFrame)
{
	const int32 Channel = BaseChannel + ChannelOffset(Role);
	if (Channel < 0 || Channel > MAX_uint16 || !HasRole(Data, Role))
	{
		return false;
	}

	OutFrame.Role = Role;
	OutFrame.Channel = Channel;
	OutFrame.Message = MessageType(Role);
	OutFrame.Payload.Reset();
	switch (Role)
	{
		case ERammsCameraStreamRole::Depth:
		{
			OutFrame.Width = Data.DepthWidth > 0 ? Data.DepthWidth : Data.Width;
			OutFrame.Height = Data.DepthHeight > 0 ? Data.DepthHeight : Data.Height;
			OutFrame.Payload.Append(reinterpret_cast<const uint8*>(Data.DepthData.GetData()),
				Data.DepthData.Num() * static_cast<int32>(sizeof(float)));
			break;
		}
		case ERammsCameraStreamRole::Motion:
		{
			// FVector2D is double; the wire is float32 x, y.
			OutFrame.Width = Data.MotionWidth;
			OutFrame.Height = Data.MotionHeight;
			const int32 Pixels = Data.MotionVectorData.Num();
			OutFrame.Payload.SetNumUninitialized(Pixels * 2 * static_cast<int32>(sizeof(float)));
			float* Out = reinterpret_cast<float*>(OutFrame.Payload.GetData());
			for (int32 Pixel = 0; Pixel < Pixels; ++Pixel)
			{
				const FVector2D& Velocity = Data.MotionVectorData[Pixel];
				Out[Pixel * 2] = static_cast<float>(Velocity.X);
				Out[Pixel * 2 + 1] = static_cast<float>(Velocity.Y);
			}
			break;
		}
		case ERammsCameraStreamRole::Mask:
		{
			OutFrame.Width = Data.ClassWidth;
			OutFrame.Height = Data.ClassHeight;
			OutFrame.Payload = Data.ClassData;
			break;
		}
		case ERammsCameraStreamRole::Color:
		default:
		{
			// FColor is B, G, R, A in memory: bgra8 as it is.
			OutFrame.Width = Data.Width;
			OutFrame.Height = Data.Height;
			OutFrame.Payload.Append(reinterpret_cast<const uint8*>(Data.ImageData.GetData()),
				Data.ImageData.Num() * static_cast<int32>(sizeof(FColor)));
			break;
		}
	}
	OutFrame.MetadataJson = BuildFrameMetadataJson(Data, StreamName, Role, Role == ERammsCameraStreamRole::Mask ? SegTable : nullptr);
	return true;
}
