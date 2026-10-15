fx fx/teleporter3.fx
{

	{										// sound
		name "sound1"
		delay 0
		sound "teleport_full_3s"		// calls 2-second long effect, sub '_1s' or '_3s' for 1- or 3-second lengths
		duration 17
	}
	{
		delay 0
		name "lightspectrum"
		duration 3
		restart 0
		light "lights/spectrumlight", 0.2, 0.02, 0.01, 500
		fadeIn 3
	}
	{
		delay 3
		name "lightspectrumoff"
		duration 2
		restart 0
		uselight "lightspectrum"
		fadeOut 1.5
	}
	{										// preamble
		delay 0	
		name "particle2"
		duration 2
		restart 0
		rotate 0.1
		model "spawn_preamble2.prt"
		offset 0, 0, 128
		fadeOut 2
	}
	{										// upper explosion
		delay 3	
		name "particle2"
		duration 2
		restart 0
		rotate 0.1
		model "spawnSphere_3a_2.prt"
		offset 0, 0, 128
		fadeOut 2
	}		   

	{										// bolt model
		delay 3.2
		name "model1"
		duration 0.75
		restart 0
		//rotate 0.1
		model "telebolt.prt"
		offset 0, 0, 0
		fadeOut 0.75
	}		   
	{										// ground strike
		delay 3.7
		name "particle2"
		duration 2.0
		restart 0
		rotate 0.1
		model "spawnSphere_3_2.prt"
		offset 0, 0, 1
		fadeOut 2.0
	}
	{										
		name "shake1"
		delay 3.7
		duration 0.5
		restart 0
		shake	125, 2.5, 256, 0, 20000
		offset	-1, 0, 0
	}
	{
		delay 3.7
		name "light5"
		duration 0.05
		restart 0
		light "lights/squareblast", 0.2, 0.02, 0.01, 300
	}
	{										// crack in floor.. could be simulaneuous with strike
		name "decal1"
		delay 3.78
		duration 1
		size 80
		restart 0
		decal "textures/decals/spawndecal"
	}
	{										// fade out the blast light
		delay 3.8
		name "light6"
		duration 0.10
		restart 0
		useLight "light5"
		fadeOut 0.005
	}
}