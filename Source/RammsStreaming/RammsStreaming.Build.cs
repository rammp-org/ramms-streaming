// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;

public class RammsStreaming : ModuleRules
{
	public RammsStreaming(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicIncludePaths.AddRange(
			new string[] {
			}
			);

		PrivateIncludePaths.AddRange(
			new string[] {
			}
			);

		PublicDependencyModuleNames.AddRange(
			new string[]
			{
				"Core",
				"CoreUObject",
				"Engine",
				"Sockets",        // FSocket, FTcpSocketBuilder, FTcpListener
				"Networking",     // FIPv4Address, FIPv4Endpoint
				"Json",           // Metadata JSON serialization
				"JsonUtilities",
				"ImageCore",      // FImageView for JPEG compression via FImageUtils
				"CameraCapture",  // FCaptureData, OnFrameCaptured delegate
			}
			);

		PrivateDependencyModuleNames.AddRange(
			new string[]
			{
			}
			);

		DynamicallyLoadedModuleNames.AddRange(
			new string[]
			{
			}
			);
	}
}
