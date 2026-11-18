// Tests for RMSS sending: the outbound queue policy, subscriptions (no frames before Subscribe), a server that never
// waits on a slow client, a port another listener holds, and the camera frames a stream source builds.
//
// The server tests run a real FRammsStreamServer on a free loopback port with real client sockets.

#include "CoreMinimal.h"
#include "CameraCaptureSegmentation.h"
#include "CameraCaptureSubsystem.h"
#include "Common/TcpSocketBuilder.h"
#include "Dom/JsonObject.h"
#include "HAL/PlatformProcess.h"
#include "Interfaces/IPv4/IPv4Endpoint.h"
#include "IPAddress.h"
#include "Misc/AutomationTest.h"
#include "RammsCameraStreams.h"
#include "RammsStreamConnection.h"
#include "RammsStreamProtocol.h"
#include "RammsStreamServer.h"
#include "RammsStreamSourceComponent.h"
#include "RammsStreamingSubsystem.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "SocketSubsystem.h"
#include "Sockets.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace RammsStreamingTests
{
	TSharedRef<FRammsStreamMessage> MakeFrame(ERammsStreamMessageType Type, uint16 Channel, int32 PayloadBytes = 16)
	{
		TSharedRef<FRammsStreamMessage> Msg = MakeShared<FRammsStreamMessage>();
		Msg->Header.MessageType = Type;
		Msg->Header.ChannelID = Channel;
		Msg->Payload.SetNumZeroed(PayloadBytes);
		Msg->Header.PayloadLen = static_cast<uint32>(PayloadBytes);
		return Msg;
	}

	/** A blocking TCP client connected to the server's loopback port, or null. */
	FSocket* ConnectClient(uint16 Port, int32 ReceiveBufferBytes)
	{
		FSocket* Socket = FTcpSocketBuilder(TEXT("RammsStreamingTestClient"))
							  .AsBlocking()
							  .WithReceiveBufferSize(ReceiveBufferBytes)
							  .Build();
		if (!Socket)
		{
			return nullptr;
		}
		const TSharedRef<FInternetAddr> Server = FIPv4Endpoint(FIPv4Address(127, 0, 0, 1), Port).ToInternetAddr();
		if (!Socket->Connect(*Server))
		{
			ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->DestroySocket(Socket);
			return nullptr;
		}
		return Socket;
	}

	void CloseClient(FSocket*& Socket)
	{
		if (Socket)
		{
			Socket->Close();
			ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->DestroySocket(Socket);
			Socket = nullptr;
		}
	}

	/** Read what has arrived (without waiting) and count the complete RMSS messages in it. */
	int32 DrainMessages(FSocket& Socket, TArray<uint8>& Buffer, TArray<FRammsStreamHeader>& OutHeaders)
	{
		uint32 Pending = 0;
		while (Socket.HasPendingData(Pending) && Pending > 0)
		{
			const int32 Old = Buffer.Num();
			Buffer.SetNumUninitialized(Old + static_cast<int32>(FMath::Min<uint32>(Pending, 4u * 1024u * 1024u)));
			int32 Read = 0;
			if (!Socket.Recv(Buffer.GetData() + Old, Buffer.Num() - Old, Read))
			{
				Buffer.SetNum(Old);
				break;
			}
			Buffer.SetNum(Old + Read);
		}
		int32 Offset = 0;
		int32 Parsed = 0;
		while (Offset < Buffer.Num())
		{
			FRammsStreamMessage Msg;
			int32				Consumed = 0;
			if (!FRammsStreamMessage::Deserialize(Buffer.GetData() + Offset, Buffer.Num() - Offset, Msg, Consumed))
			{
				break;
			}
			OutHeaders.Add(Msg.Header);
			Offset += Consumed;
			++Parsed;
		}
		Buffer.RemoveAt(0, Offset);
		return Parsed;
	}

	bool WaitFor(TFunctionRef<bool()> Condition, double TimeoutSeconds)
	{
		const double Deadline = FPlatformTime::Seconds() + TimeoutSeconds;
		while (!Condition())
		{
			if (FPlatformTime::Seconds() > Deadline)
			{
				return false;
			}
			FPlatformProcess::Sleep(0.005f);
		}
		return true;
	}

	TSharedPtr<FJsonObject> ParseJson(const FString& Json)
	{
		TSharedPtr<FJsonObject>			Obj;
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
		return FJsonSerializer::Deserialize(Reader, Obj) ? Obj : nullptr;
	}

	/** Send one control message (Subscribe, Unsubscribe, Ping) with this metadata JSON from a client. */
	bool SendControl(FSocket& Socket, ERammsStreamMessageType Type, const FString& Json)
	{
		FRammsStreamMessage Msg;
		Msg.Header.MessageType = Type;
		if (!Json.IsEmpty())
		{
			Msg.SetMetadataString(Json);
		}
		const TArray<uint8> Bytes = Msg.Serialize();
		int32				Total = 0;
		while (Total < Bytes.Num())
		{
			int32 Sent = 0;
			if (!Socket.Send(Bytes.GetData() + Total, Bytes.Num() - Total, Sent) || Sent <= 0)
			{
				return false;
			}
			Total += Sent;
		}
		return true;
	}

	int32 CountOf(const TArray<FRammsStreamHeader>& Headers, ERammsStreamMessageType Type)
	{
		int32 Count = 0;
		for (const FRammsStreamHeader& Header : Headers)
		{
			Count += Header.MessageType == Type ? 1 : 0;
		}
		return Count;
	}

	/**
	 * Send a control message, then a Ping, and tick the server until the client has the Ping back: the server
	 * handles one client's messages in order, so the first one has been applied by then.
	 */
	bool Request(FRammsStreamServer& Server, FSocket& Client, ERammsStreamMessageType Type, const FString& Json,
		TArray<uint8>& Buffer, TArray<FRammsStreamHeader>& Received)
	{
		const int32 PongsBefore = CountOf(Received, ERammsStreamMessageType::Ping);
		if ((Type != ERammsStreamMessageType::Ping && !SendControl(Client, Type, Json))
			|| !SendControl(Client, ERammsStreamMessageType::Ping, FString()))
		{
			return false;
		}
		return WaitFor([&]() {
			Server.Tick();
			DrainMessages(Client, Buffer, Received);
			return CountOf(Received, ERammsStreamMessageType::Ping) > PongsBefore;
		},
			3.0);
	}

	/** The frames (not acks or pongs) among Headers on Channel. */
	int32 FramesOn(const TArray<FRammsStreamHeader>& Headers, uint16 Channel)
	{
		int32 Count = 0;
		for (const FRammsStreamHeader& Header : Headers)
		{
			Count += (Header.ChannelID == Channel && GetFrameCategory(Header.MessageType) != ERammsFrameCategory::Control) ? 1 : 0;
		}
		return Count;
	}
} // namespace RammsStreamingTests

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRammsStreamingQueueLatestPerStream, "Ramms.Streaming.Queue.LatestPerStream",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FRammsStreamingQueueLatestPerStream::RunTest(const FString& Parameters)
{
	using namespace RammsStreamingTests;

	// No socket: only the queue is exercised.
	FRammsStreamConnection					Conn(nullptr, 1);
	TArray<TSharedRef<FRammsStreamMessage>> Queued;

	// One camera's four streams are four channels: none pushes another out.
	int32 Dropped = 0;
	Conn.EnqueueOutbound(MakeFrame(ERammsStreamMessageType::FrameRGB, 0), true, &Dropped);
	Conn.EnqueueOutbound(MakeFrame(ERammsStreamMessageType::FrameDepth, 100), true, &Dropped);
	Conn.EnqueueOutbound(MakeFrame(ERammsStreamMessageType::FrameMotion, 200), true, &Dropped);
	Conn.EnqueueOutbound(MakeFrame(ERammsStreamMessageType::FrameData, 300), true, &Dropped);
	Conn.GetQueuedOutbound(Queued);
	TestEqual(TEXT("all four streams of a camera queued"), Queued.Num(), 4);
	TestEqual(TEXT("nothing dropped"), Dropped, 0);
	TestTrue(TEXT("colour kept"), Queued.Num() > 0 && Queued[0]->Header.ChannelID == 0);

	// A newer frame of a stream takes the waiting one's place -- in place, so the stream keeps its turn. Moved to the
	// back instead, the stream queued last in each tick (the mask) would never be reached by a client that can take
	// only part of a tick's frames.
	const TSharedRef<FRammsStreamMessage> NewerDepth = MakeFrame(ERammsStreamMessageType::FrameDepth, 100);
	Conn.EnqueueOutbound(NewerDepth, true, &Dropped);
	Conn.GetQueuedOutbound(Queued);
	TestEqual(TEXT("replaced, not added"), Queued.Num(), 4);
	TestEqual(TEXT("the older depth frame dropped"), Dropped, 1);
	TestTrue(TEXT("the newer depth frame in the older one's place"), Queued.Num() == 4 && &Queued[1].Get() == &NewerDepth.Get());
	TestTrue(TEXT("the streams queued after it keep their turn"),
		Queued.Num() == 4 && Queued[2]->Header.ChannelID == 200 && Queued[3]->Header.ChannelID == 300);

	// Several cameras on one channel (a source with a substring filter) are different streams: their stream keys
	// keep them apart, and each keeps its latest frame.
	FRammsStreamConnection				  Shared(nullptr, 4);
	const uint64						  FrontKey = URammsStreamingSubsystem::MakeStreamKey(TEXT("camera/front/color"), TEXT("/A"), TEXT("A::front"));
	const uint64						  RearKey = URammsStreamingSubsystem::MakeStreamKey(TEXT("camera/rear/color"), TEXT("/A"), TEXT("A::rear"));
	const TSharedRef<FRammsStreamMessage> Front = MakeFrame(ERammsStreamMessageType::FrameRGB, 5);
	const TSharedRef<FRammsStreamMessage> Rear = MakeFrame(ERammsStreamMessageType::FrameRGB, 5);
	TestTrue(TEXT("two cameras, two keys"), FrontKey != 0 && RearKey != 0 && FrontKey != RearKey);
	Shared.EnqueueOutbound(Front, true, &Dropped, FrontKey);
	Shared.EnqueueOutbound(Rear, true, &Dropped, RearKey);
	Shared.GetQueuedOutbound(Queued);
	TestEqual(TEXT("both cameras queued on the one channel"), Queued.Num(), 2);
	TestEqual(TEXT("the second camera dropped nothing"), Dropped, 0);
	const TSharedRef<FRammsStreamMessage> NewerFront = MakeFrame(ERammsStreamMessageType::FrameRGB, 5);
	Shared.EnqueueOutbound(NewerFront, true, &Dropped, FrontKey);
	Shared.GetQueuedOutbound(Queued);
	TestTrue(TEXT("a camera's newer frame replaces only its own"),
		Queued.Num() == 2 && &Queued[0].Get() == &NewerFront.Get() && &Queued[1].Get() == &Rear.Get());

	// Control messages are never replaced or pushed out.
	FRammsStreamConnection Small(nullptr, 2);
	Small.MaxOutboundQueueSize = 2;
	Small.EnqueueOutbound(MakeFrame(ERammsStreamMessageType::Ack, 0, 0), true, &Dropped);
	Small.EnqueueOutbound(MakeFrame(ERammsStreamMessageType::Ack, 0, 0), true, &Dropped);
	Small.GetQueuedOutbound(Queued);
	TestEqual(TEXT("two acks on one channel both kept"), Queued.Num(), 2);
	TestFalse(TEXT("a queue full of acks refuses a frame"), Small.EnqueueOutbound(MakeFrame(ERammsStreamMessageType::FrameRGB, 1), true, &Dropped));
	Small.GetQueuedOutbound(Queued);
	TestTrue(TEXT("and keeps the acks"), Queued.Num() == 2 && Queued[0]->Header.MessageType == ERammsStreamMessageType::Ack && Queued[1]->Header.MessageType == ERammsStreamMessageType::Ack);
	TestEqual(TEXT("the refused frame counts as dropped"), Dropped, 1);

	// Full: the oldest frame makes room, an ack stays.
	FRammsStreamConnection Full(nullptr, 3);
	Full.MaxOutboundQueueSize = 3;
	Full.EnqueueOutbound(MakeFrame(ERammsStreamMessageType::Ack, 0, 0));
	Full.EnqueueOutbound(MakeFrame(ERammsStreamMessageType::FrameRGB, 1));
	Full.EnqueueOutbound(MakeFrame(ERammsStreamMessageType::FrameRGB, 2));
	Full.EnqueueOutbound(MakeFrame(ERammsStreamMessageType::FrameRGB, 3), true, &Dropped);
	Full.GetQueuedOutbound(Queued);
	TestEqual(TEXT("at capacity"), Queued.Num(), 3);
	TestTrue(TEXT("the ack kept"), Queued.Num() == 3 && Queued[0]->Header.MessageType == ERammsStreamMessageType::Ack);
	TestTrue(TEXT("the oldest frame (channel 1) dropped"), Queued.Num() == 3 && Queued[1]->Header.ChannelID == 2 && Queued[2]->Header.ChannelID == 3);

	// Without bDropOldest a full queue refuses.
	TestFalse(TEXT("refused when full and not dropping"), Full.EnqueueOutbound(MakeFrame(ERammsStreamMessageType::FrameRGB, 4), false, &Dropped));
	TestEqual(TEXT("the refused message counts as dropped"), Dropped, 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRammsStreamingSubscriptions, "Ramms.Streaming.Server.NoFramesBeforeSubscribe",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FRammsStreamingSubscriptions::RunTest(const FString& Parameters)
{
	using namespace RammsStreamingTests;
	// The viewer stops reading at the end; should the machine be slow enough for that to count as a stall, say so.
	AddExpectedMessagePlain(TEXT("RMSS: Dropping connection"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, -1);

	FRammsStreamServer Server(0, 4);
	if (!TestTrue(TEXT("server listens on a free port"), Server.StartListening()))
	{
		return false;
	}
	const uint16 Port = Server.GetPort();

	// Sender only sends (an image feed into UE: it never subscribes and never reads frames); Viewer subscribes.
	FSocket* Sender = ConnectClient(Port, 64 * 1024);
	FSocket* Viewer = ConnectClient(Port, 4 * 1024 * 1024);
	if (!TestNotNull(TEXT("sender connects"), Sender) || !TestNotNull(TEXT("viewer connects"), Viewer))
	{
		CloseClient(Sender);
		CloseClient(Viewer);
		Server.StopListening();
		return false;
	}
	TestTrue(TEXT("both clients accepted"), WaitFor([&Server]() { return Server.GetConnectionCount() == 2; }, 3.0));

	TArray<uint8>			   SenderBuffer, ViewerBuffer;
	TArray<FRammsStreamHeader> SenderGot, ViewerGot;
	const auto				   Broadcast = [&Server](uint16 Channel, int32 Bytes) {
		Server.BroadcastToSubscribers(MakeFrame(ERammsStreamMessageType::FrameRGB, Channel, Bytes));
	};

	// Nobody has subscribed: no channel is wanted, and a frame goes to nobody.
	TestFalse(TEXT("no subscriber before any Subscribe"), Server.HasSubscriber(1));
	Broadcast(1, 1024);
	FPlatformProcess::Sleep(0.2f);
	uint32 Pending = 0;
	TestTrue(TEXT("the viewer is sent nothing before it subscribes"), !Viewer->HasPendingData(Pending) || Pending == 0);

	// A client that never subscribed still gets the replies to its own requests.
	TestTrue(TEXT("a pong to a client that never subscribed"), Request(Server, *Sender, ERammsStreamMessageType::Ping, FString(), SenderBuffer, SenderGot));

	// Subscribed to channel 1: that channel only.
	TestTrue(TEXT("subscribe to channel 1"), Request(Server, *Viewer, ERammsStreamMessageType::Subscribe, TEXT("{\"channels\": [1]}"), ViewerBuffer, ViewerGot));
	TestEqual(TEXT("the subscription is acknowledged"), CountOf(ViewerGot, ERammsStreamMessageType::Ack), 1);
	TestTrue(TEXT("channel 1 wanted"), Server.HasSubscriber(1));
	TestFalse(TEXT("channel 2 not"), Server.HasSubscriber(2));
	Broadcast(2, 1024);
	Broadcast(1, 1024);
	TestTrue(TEXT("the viewer gets its channel"), WaitFor([&]() {
		DrainMessages(*Viewer, ViewerBuffer, ViewerGot);
		return FramesOn(ViewerGot, 1) > 0;
	},
													  3.0));
	TestEqual(TEXT("and not another"), FramesOn(ViewerGot, 2), 0);

	// Unsubscribing the last listed channel leaves nothing, never "every channel".
	TestTrue(TEXT("unsubscribe channel 1"), Request(Server, *Viewer, ERammsStreamMessageType::Unsubscribe, TEXT("{\"channels\": [1]}"), ViewerBuffer, ViewerGot));
	TestFalse(TEXT("channel 1 no longer wanted"), Server.HasSubscriber(1));
	TestFalse(TEXT("nor any other"), Server.HasSubscriber(2));

	// Subscribe without a list (an empty one alike): every channel; Unsubscribe with a list takes those out of it.
	TestTrue(TEXT("subscribe to everything"), Request(Server, *Viewer, ERammsStreamMessageType::Subscribe, TEXT("{\"channels\": []}"), ViewerBuffer, ViewerGot));
	TestTrue(TEXT("every channel wanted"), Server.HasSubscriber(2) && Server.HasSubscriber(7) && Server.HasSubscriber(65535));
	TestTrue(TEXT("unsubscribe channel 7"), Request(Server, *Viewer, ERammsStreamMessageType::Unsubscribe, TEXT("{\"channels\": [7]}"), ViewerBuffer, ViewerGot));
	TestFalse(TEXT("channel 7 taken out"), Server.HasSubscriber(7));
	TestTrue(TEXT("the rest still wanted"), Server.HasSubscriber(2));
	TestTrue(TEXT("subscribe channel 7 again"), Request(Server, *Viewer, ERammsStreamMessageType::Subscribe, TEXT("{\"channels\": [7]}"), ViewerBuffer, ViewerGot));
	TestTrue(TEXT("channel 7 back"), Server.HasSubscriber(7));

	// Unsubscribe without a list: nothing at all.
	TestTrue(TEXT("unsubscribe everything"), Request(Server, *Viewer, ERammsStreamMessageType::Unsubscribe, TEXT("{}"), ViewerBuffer, ViewerGot));
	TestFalse(TEXT("nothing wanted"), Server.HasSubscriber(2) || Server.HasSubscriber(7));

	// A Subscribe with no metadata at all reads as {}: every channel.
	TestTrue(TEXT("bare subscribe"), Request(Server, *Viewer, ERammsStreamMessageType::Subscribe, FString(), ViewerBuffer, ViewerGot));
	TestTrue(TEXT("every channel wanted again"), Server.HasSubscriber(3));

	// Megabytes of frames later the sender, which never subscribed and reads nothing, has been queued nothing: it is
	// neither filled up nor dropped for not reading.
	for (int32 i = 0; i < 20; ++i)
	{
		Broadcast(1, 2 * 1024 * 1024);
		DrainMessages(*Viewer, ViewerBuffer, ViewerGot);
		FPlatformProcess::Sleep(0.002f);
	}
	FPlatformProcess::Sleep(0.2f);
	Server.Tick();
	Pending = 0;
	TestTrue(TEXT("the sender was sent no frame"), !Sender->HasPendingData(Pending) || Pending == 0);
	TestEqual(TEXT("the sender is still connected"), Server.GetConnectionCount(), 2);

	// The statistics count both ways.
	TestTrue(TEXT("bytes sent are counted"), Server.GetStats().TotalBytesSent.Load() > 0);
	TestTrue(TEXT("bytes received are counted"), Server.GetStats().TotalBytesReceived.Load() > 0);

	// This run subscribes, unsubscribes and pings, and never sends an inbound FRAME: the two
	// counters have to disagree, or FramesReceived is counting control traffic and a client that
	// only keeps its connection alive would read as a robot streaming.
	TestTrue(TEXT("inbound control messages are counted"), Server.GetStats().MessagesReceived.Load() > 0);
	TestEqual(TEXT("control messages are not counted as frames"),
		static_cast<int32>(Server.GetStats().FramesReceived.Load()), 0);

	Server.StopListening();
	CloseClient(Sender);
	CloseClient(Viewer);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRammsStreamingServerSlowClient, "Ramms.Streaming.Server.SlowClientDoesNotBlock",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FRammsStreamingServerSlowClient::RunTest(const FString& Parameters)
{
	using namespace RammsStreamingTests;
	// A send interrupted by StopListening is not an error, but say it if it happens.
	AddExpectedMessagePlain(TEXT("RMSS: Dropping connection"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, -1);

	FRammsStreamServer Server(0, 4);
	if (!TestTrue(TEXT("server listens on a free port"), Server.StartListening()))
	{
		return false;
	}
	const uint16 Port = Server.GetPort();
	TestTrue(TEXT("the bound port is reported"), Port != 0);

	// Stalled subscribes but never reads a frame; Reader does.
	FSocket* Stalled = ConnectClient(Port, 8 * 1024);
	FSocket* Reader = ConnectClient(Port, 4 * 1024 * 1024);
	if (!TestNotNull(TEXT("stalled client connects"), Stalled) || !TestNotNull(TEXT("reading client connects"), Reader))
	{
		CloseClient(Stalled);
		CloseClient(Reader);
		Server.StopListening();
		return false;
	}
	TestTrue(TEXT("both clients accepted"), WaitFor([&Server]() { return Server.GetConnectionCount() == 2; }, 3.0));

	// Both subscribe to channel 1 (frames go only to clients that subscribed).
	TArray<uint8>			   ReaderBuffer, StalledBuffer;
	TArray<FRammsStreamHeader> Received, StalledReceived;
	TestTrue(TEXT("the stalled client subscribes"),
		Request(Server, *Stalled, ERammsStreamMessageType::Subscribe, TEXT("{\"channels\": [1]}"), StalledBuffer, StalledReceived));
	TestTrue(TEXT("the reading client subscribes"),
		Request(Server, *Reader, ERammsStreamMessageType::Subscribe, TEXT("{\"channels\": [1]}"), ReaderBuffer, Received));
	Received.Reset();

	// Far more than the stalled client's socket buffers hold. Nothing on the game thread may wait for it.
	constexpr int32 FrameBytes = 2 * 1024 * 1024;
	constexpr int32 Frames = 40;
	double			LongestCall = 0.0;
	for (int32 i = 0; i < Frames; ++i)
	{
		const TSharedRef<FRammsStreamMessage> Frame = MakeFrame(ERammsStreamMessageType::FrameRGB, 1, FrameBytes);
		Frame->Header.SequenceNum = static_cast<uint32>(i);

		double Start = FPlatformTime::Seconds();
		Server.BroadcastToSubscribers(Frame);
		LongestCall = FMath::Max(LongestCall, FPlatformTime::Seconds() - Start);

		Start = FPlatformTime::Seconds();
		Server.Tick();
		Server.GetConnectionCount();
		Server.HasSubscriber(1);
		LongestCall = FMath::Max(LongestCall, FPlatformTime::Seconds() - Start);

		DrainMessages(*Reader, ReaderBuffer, Received);
		FPlatformProcess::Sleep(0.005f);
	}
	TestTrue(FString::Printf(TEXT("broadcast / tick never wait on a client (longest call %.3f s)"), LongestCall), LongestCall < 0.25);

	// The reading client is served while the other is stalled.
	WaitFor([&]() {
		DrainMessages(*Reader, ReaderBuffer, Received);
		return Received.Num() > 0 && Received.Last().SequenceNum == static_cast<uint32>(Frames - 1);
	},
		5.0);
	TestTrue(FString::Printf(TEXT("the reading client received frames (%d)"), Received.Num()), Received.Num() >= 3);
	TestTrue(TEXT("and the latest one"), Received.Num() > 0 && Received.Last().SequenceNum == static_cast<uint32>(Frames - 1));
	bool bInOrder = true;
	for (int32 i = 1; i < Received.Num(); ++i)
	{
		bInOrder &= Received[i].SequenceNum > Received[i - 1].SequenceNum && Received[i].ChannelID == 1;
	}
	TestTrue(TEXT("whole frames, in order"), bInOrder);

	// Stopping does not wait on the stalled client either.
	const double StopStart = FPlatformTime::Seconds();
	Server.StopListening();
	const double StopSeconds = FPlatformTime::Seconds() - StopStart;
	TestTrue(FString::Printf(TEXT("StopListening is quick with a stalled client (%.3f s)"), StopSeconds), StopSeconds < 2.0);
	TestEqual(TEXT("no connection outlives StopListening"), Server.GetConnectionCount(), 0);

	CloseClient(Stalled);
	CloseClient(Reader);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRammsStreamingServerTakenPort, "Ramms.Streaming.Server.RefusesTakenPort",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FRammsStreamingServerTakenPort::RunTest(const FString& Parameters)
{
	AddExpectedMessagePlain(TEXT("RMSS: "), ELogVerbosity::Error, EAutomationExpectedMessageFlags::Contains, 0);

	FRammsStreamServer First(0, 2);
	if (!TestTrue(TEXT("first server listens"), First.StartListening()))
	{
		return false;
	}
	FRammsStreamServer Second(First.GetPort(), 2);
	TestFalse(TEXT("a second server does not share the port"), Second.StartListening());
	TestFalse(TEXT("and says why"), Second.GetLastError().IsEmpty());
	TestFalse(TEXT("the second is not listening"), Second.IsListening());
	First.StopListening();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRammsStreamingCameraFrames, "Ramms.Streaming.CameraFrames.Build",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FRammsStreamingCameraFrames::RunTest(const FString& Parameters)
{
	using namespace RammsStreamingTests;

	// 4x2 colour, 2x1 depth, 2x1 motion, 4x2 class mask.
	FCaptureData Data;
	Data.CameraID.ActorName = TEXT("Robot_C_0");
	Data.CameraID.ComponentName = TEXT("front");
	Data.CameraID.UniqueID = TEXT("Robot_C_0::front");
	Data.ActorPath = TEXT("/Temp/Map.Map:PersistentLevel.Robot_C_0");
	Data.Width = 4;
	Data.Height = 2;
	Data.ImageData.Init(FColor(10, 20, 30, 255), 8);
	Data.DepthWidth = 2;
	Data.DepthHeight = 1;
	Data.DepthData = { 150.0f, 250.0f };
	Data.MotionWidth = 2;
	Data.MotionHeight = 1;
	Data.MotionVectorData = { FVector2D(1.5, -2.0), FVector2D(0.25, 4.0) };
	Data.ClassWidth = 4;
	Data.ClassHeight = 2;
	Data.ClassData = { 0, 3, 3, 0, 7, 0, 0, 0 };

	FCameraCaptureSegTable Table;
	Table.FineNames[3] = FName(TEXT("Car"));
	Table.FineNames[7] = FName(TEXT("Bench"));

	constexpr int32 Base = 5;
	struct FExpect
	{
		ERammsCameraStreamRole	Role;
		int32					Channel;
		ERammsStreamMessageType Message;
		int32					W, H, Bytes;
		const TCHAR*			Fmt;
	};
	const FExpect Expected[] = {
		{ ERammsCameraStreamRole::Color, Base, ERammsStreamMessageType::FrameRGB, 4, 2, 4 * 2 * 4, TEXT("bgra8") },
		{ ERammsCameraStreamRole::Depth, Base + 100, ERammsStreamMessageType::FrameDepth, 2, 1, 2 * 4, TEXT("float32") },
		{ ERammsCameraStreamRole::Motion, Base + 200, ERammsStreamMessageType::FrameMotion, 2, 1, 2 * 8, TEXT("rg32f") },
		{ ERammsCameraStreamRole::Mask, Base + 300, ERammsStreamMessageType::FrameData, 4, 2, 4 * 2, TEXT("mono8") },
	};
	TSet<uint64> Keys;
	for (const FExpect& E : Expected)
	{
		const FString		 Role = FRammsCameraStreams::RoleName(E.Role);
		FRammsCameraFrameOut Frame;
		if (!TestTrue(Role + TEXT(": built"), FRammsCameraStreams::BuildFrame(Data, TEXT("front"), E.Role, Base, &Table, Frame)))
		{
			continue;
		}
		TestEqual(Role + TEXT(": channel"), Frame.Channel, E.Channel);
		TestTrue(Role + TEXT(": message type"), Frame.Message == E.Message);
		TestEqual(Role + TEXT(": width"), Frame.Width, E.W);
		TestEqual(Role + TEXT(": height"), Frame.Height, E.H);
		TestEqual(Role + TEXT(": payload bytes"), Frame.Payload.Num(), E.Bytes);

		// The stream key: the same one the frame's own metadata gives, and one per role.
		TestTrue(Role + TEXT(": has a stream key"), Frame.StreamKey != 0);
		TestTrue(Role + TEXT(": the key its metadata gives"), URammsStreamingSubsystem::StreamKeyFromMetadata(Frame.MetadataJson) == Frame.StreamKey);
		Keys.Add(Frame.StreamKey);

		const TSharedPtr<FJsonObject> Meta = ParseJson(Frame.MetadataJson);
		if (!TestTrue(Role + TEXT(": metadata is JSON"), Meta.IsValid()))
		{
			continue;
		}
		TestEqual(Role + TEXT(": w is the payload grid"), static_cast<int32>(Meta->GetNumberField(TEXT("w"))), E.W);
		TestEqual(Role + TEXT(": h is the payload grid"), static_cast<int32>(Meta->GetNumberField(TEXT("h"))), E.H);
		TestEqual(Role + TEXT(": fmt"), Meta->GetStringField(TEXT("fmt")), FString(E.Fmt));
		TestEqual(Role + TEXT(": actor is the owner's path"), Meta->GetStringField(TEXT("actor")), Data.ActorPath);
		TestEqual(Role + TEXT(": stream_id"), Meta->GetStringField(TEXT("stream_id")), FRammsCameraStreams::StreamId(TEXT("front"), E.Role));
		TestEqual(Role + TEXT(": group"), Meta->GetStringField(TEXT("group")), FString(TEXT("front")));
	}
	TestEqual(TEXT("four roles, four stream keys"), Keys.Num(), 4);

	// Another camera's colour on the same channel is another stream.
	FCaptureData Other = Data;
	Other.CameraID.ComponentName = TEXT("rear");
	Other.CameraID.UniqueID = TEXT("Robot_C_0::rear");
	FRammsCameraFrameOut FrontColor, RearColor;
	if (FRammsCameraStreams::BuildFrame(Data, TEXT("front"), ERammsCameraStreamRole::Color, Base, nullptr, FrontColor)
		&& FRammsCameraStreams::BuildFrame(Other, TEXT("rear"), ERammsCameraStreamRole::Color, Base, nullptr, RearColor))
	{
		TestTrue(TEXT("two cameras on one channel: two stream keys"), FrontColor.Channel == RearColor.Channel && FrontColor.StreamKey != RearColor.StreamKey);
	}
	TestTrue(TEXT("no metadata, no stream key"), URammsStreamingSubsystem::StreamKeyFromMetadata(FString()) == 0);
	TestTrue(TEXT("metadata without an identity, no stream key"), URammsStreamingSubsystem::StreamKeyFromMetadata(TEXT("{\"w\": 4}")) == 0);

	// The payloads hold the planes, in the wire formats.
	FRammsCameraFrameOut Motion;
	if (FRammsCameraStreams::BuildFrame(Data, TEXT("front"), ERammsCameraStreamRole::Motion, Base, nullptr, Motion) && Motion.Payload.Num() == 16)
	{
		const float* V = reinterpret_cast<const float*>(Motion.Payload.GetData());
		TestTrue(TEXT("motion: interleaved float32 x, y"), V[0] == 1.5f && V[1] == -2.0f && V[2] == 0.25f && V[3] == 4.0f);
	}
	FRammsCameraFrameOut Depth;
	if (FRammsCameraStreams::BuildFrame(Data, TEXT("front"), ERammsCameraStreamRole::Depth, Base, nullptr, Depth) && Depth.Payload.Num() == 8)
	{
		const float* D = reinterpret_cast<const float*>(Depth.Payload.GetData());
		TestTrue(TEXT("depth: float32 cm"), D[0] == 150.0f && D[1] == 250.0f);
		const TSharedPtr<FJsonObject> Meta = ParseJson(Depth.MetadataJson);
		TestTrue(TEXT("depth: unit cm"), Meta.IsValid() && Meta->GetStringField(TEXT("unit")) == TEXT("cm"));
	}
	FRammsCameraFrameOut Color;
	if (FRammsCameraStreams::BuildFrame(Data, TEXT("front"), ERammsCameraStreamRole::Color, Base, nullptr, Color) && Color.Payload.Num() == 32)
	{
		TestTrue(TEXT("colour: B, G, R, A bytes"), Color.Payload[0] == 30 && Color.Payload[1] == 20 && Color.Payload[2] == 10 && Color.Payload[3] == 255);
	}
	FRammsCameraFrameOut Mask;
	if (FRammsCameraStreams::BuildFrame(Data, TEXT("front"), ERammsCameraStreamRole::Mask, Base, &Table, Mask))
	{
		TestTrue(TEXT("mask: the class values"), Mask.Payload == Data.ClassData);
		const TSharedPtr<FJsonObject>  Meta = ParseJson(Mask.MetadataJson);
		const TSharedPtr<FJsonObject>* Classes = nullptr;
		TestTrue(TEXT("mask: the class table"), Meta.IsValid() && Meta->TryGetObjectField(TEXT("classes"), Classes) && Classes && (*Classes)->GetStringField(TEXT("3")) == TEXT("Car"));
		TestTrue(TEXT("mask: NumSegmentIDs from the table"), Meta.IsValid() && static_cast<int32>(Meta->GetObjectField(TEXT("MaterialScalarParameters"))->GetNumberField(TEXT("NumSegmentIDs"))) == 8);
	}

	// A reserved stencil value (something else's custom depth, RammsUI's projector surfaces say) shows up in the mask:
	// NumSegmentIDs covers it and the metadata lists it.
	Table.Reserved.Add(200);
	TestEqual(TEXT("NumSegmentIds covers a reserved value"), FRammsCameraStreams::NumSegmentIds(Table), 201);
	FCaptureData WithReserved = Data;
	WithReserved.ClassData[6] = 200;
	FRammsCameraFrameOut ReservedMask;
	if (TestTrue(TEXT("mask with a reserved value built"), FRammsCameraStreams::BuildFrame(WithReserved, TEXT("front"), ERammsCameraStreamRole::Mask, Base, &Table, ReservedMask)))
	{
		const TSharedPtr<FJsonObject> Meta = ParseJson(ReservedMask.MetadataJson);
		TestTrue(TEXT("NumSegmentIDs above every mask value"),
			Meta.IsValid() && static_cast<int32>(Meta->GetObjectField(TEXT("MaterialScalarParameters"))->GetNumberField(TEXT("NumSegmentIDs"))) == 201);
		const TArray<TSharedPtr<FJsonValue>>* Reserved = nullptr;
		TestTrue(TEXT("reserved values listed"), Meta.IsValid() && Meta->TryGetArrayField(TEXT("reserved"), Reserved) && Reserved && Reserved->Num() == 1 && static_cast<int32>((*Reserved)[0]->AsNumber()) == 200);
	}
	// A value neither the table nor its reserved values know is still below NumSegmentIDs.
	FCaptureData Stray = Data;
	Stray.ClassData[6] = 250;
	FRammsCameraFrameOut StrayMask;
	if (FRammsCameraStreams::BuildFrame(Stray, TEXT("front"), ERammsCameraStreamRole::Mask, Base, &Table, StrayMask))
	{
		const TSharedPtr<FJsonObject> Meta = ParseJson(StrayMask.MetadataJson);
		TestTrue(TEXT("NumSegmentIDs covers a stray value"),
			Meta.IsValid() && static_cast<int32>(Meta->GetObjectField(TEXT("MaterialScalarParameters"))->GetNumberField(TEXT("NumSegmentIDs"))) == 251);
	}

	// Planes the capture did not produce are not built; nor a role whose channel would not fit the header.
	FCaptureData		 Bare;
	FRammsCameraFrameOut Unused;
	TestFalse(TEXT("no motion plane: no motion frame"), FRammsCameraStreams::BuildFrame(Bare, TEXT("x"), ERammsCameraStreamRole::Motion, 0, nullptr, Unused));
	TestFalse(TEXT("no mask: no mask frame"), FRammsCameraStreams::BuildFrame(Bare, TEXT("x"), ERammsCameraStreamRole::Mask, 0, nullptr, Unused));
	TestFalse(TEXT("mask channel past 65535"), FRammsCameraStreams::BuildFrame(Data, TEXT("x"), ERammsCameraStreamRole::Mask, 65300, nullptr, Unused));
	TestTrue(TEXT("colour on 65300 still fits"), FRammsCameraStreams::BuildFrame(Data, TEXT("x"), ERammsCameraStreamRole::Color, 65300, nullptr, Unused));
	TestTrue(TEXT("base channel range"), FRammsCameraStreams::IsValidBaseChannel(65235) && !FRammsCameraStreams::IsValidBaseChannel(65236) && !FRammsCameraStreams::IsValidBaseChannel(-1));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRammsStreamingSourceStreamName, "Ramms.Streaming.Source.StreamName",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FRammsStreamingSourceStreamName::RunTest(const FString& Parameters)
{
	URammsStreamSourceComponent* Source = NewObject<URammsStreamSourceComponent>();
	FCameraIdentifier			 Camera;
	Camera.ActorName = TEXT("Robot_C_0");
	Camera.ComponentName = TEXT("front_wide");
	Camera.UniqueID = TEXT("Robot_C_0::front_wide");

	Source->StreamName = TEXT("front");
	Source->CameraFilter = TEXT("front");
	Source->bExactCameraFilter = false;
	TestEqual(TEXT("a substring filter (several cameras) streams under the component name"), Source->GetStreamNameFor(Camera), FString(TEXT("front_wide")));
	Source->bExactCameraFilter = true;
	TestEqual(TEXT("an exact filter (one camera) streams under StreamName"), Source->GetStreamNameFor(Camera), FString(TEXT("front")));
	Source->StreamName.Reset();
	TestEqual(TEXT("no StreamName: the component name"), Source->GetStreamNameFor(Camera), FString(TEXT("front_wide")));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
