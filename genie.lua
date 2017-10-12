project "lumixengine_turbobadger"
	libType()
	files { 
		"src/**.c",
		"src/**.cpp",
		"src/**.h",
		"genie.lua"
	}
	includedirs { 
		"../lumixengine_turbobadger/src", 
		"../lumixengine_turbobadger/src/tb",
		"../lumixengine/external/bgfx/include", 
		"../lumixengine/external/lua/include", 
		"../lumixengine/external/imgui", 
	}
	buildoptions { "/wd4267", "/wd4068", "/wd4244" }
	defines { "BUILDING_TURBOBADGER", "_CRT_SECURE_NO_WARNINGS" }
	links { "engine" }
	useLua()
	defaultConfigurations()
