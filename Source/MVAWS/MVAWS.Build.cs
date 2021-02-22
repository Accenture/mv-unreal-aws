/* (c) 2021 by Mackevision Mackevision Medien Design GmbH
 * Licensed under the Apache License, Version 2.0.
 * See attached file LICENSE for full details
 */
using System.IO;
using UnrealBuildTool;

public class MVAWS : ModuleRules {

	public MVAWS(ReadOnlyTargetRules Target) : base(Target) {

        PrivatePCHHeaderFile = "Private/MVAWSPrivatePCH.h";

		// Wanna try C++ 17 std
		CppStandard = CppStandardVersion.Cpp17;
		PrivateDefinitions.Add("_SILENCE_CXX17_OLD_ALLOCATOR_MEMBERS_DEPRECATION_WARNING");
		PrivateDefinitions.Add("_SILENCE_CXX17_STRSTREAM_DEPRECATION_WARNING");

		PublicDependencyModuleNames.AddRange(new string[] {
            "Core",
            "CoreUObject",
            "Engine",
            "InputCore",
            "Json",
            "JsonUtilities"
        });

        PrivateDependencyModuleNames.AddRange(new string[] { 
			"AWSSDK",
			"HTTP"
		});
    }
}
