fx fx/teleporter1_fly
{

	{										// sound
		name "sound1"
		delay 0
		sound "teleport_full_1s"		// calls 2-second long effect, sub '_1s' or '_3s' for 1- or 3-second lengths
		duration 17
	}
	
	{
		delay 0
		name "lightspectrum"
		duration 1
		restart 0
		light "lights/spectrumlight", 0.2, 0.02, 0.01, 500
		fadeIn 1
	}
	{
		delay 1
		name "lightspectrumoff"
		duration 2
		restart 0
		uselight "lightspectrum"
		fadeOut 1.5
	}
	
	{										// rain particles
		delay 0
		name "particle1"
		duration 2.0
		restart 0
		model "spawnSphere_2.prt"
		offset 0, 0, 0
		fadeOut 2
	}
	{										// upper explosion
		delay 1.7
		name "particle2"
		duration 2
		restart 0
		rotate 0.1
		model "spawnSphere_2a.prt"
		offset 0, 0, 0
		fadeOut 2
	}		   
	{										
		name "shake1"
		delay 1.7
		duration 0.5
		restart 0
		shake	125, 2.5, 256, 0, 20000
		offset	-1, 0, 0
	}
	{
		delay 1.7
		name "light5"
		duration 0.05
		restart 0
		light "lights/squareblast", 0.2, 0.02, 0.01, 300
	}
	{										// fade out the blast light
		delay 1.8
		name "light6"
		duration 0.10
		restart 0
		useLight "light5"
		fadeOut 0.005
	}
}

fx fx/teleporter2_fly
{

	{										// sound
		name "sound1"
		delay 0
		sound "teleport_full_2s"		// calls 2-second long effect, sub '_1s' or '_3s' for 1- or 3-second lengths
		duration 17
	}
	
	{
		delay 0
		name "lightspectrum"
		duration 1
		restart 0
		light "lights/spectrumlight", 0.2, 0.02, 0.01, 500
		fadeIn 1
	}
	{
		delay 2
		name "lightspectrumoff"
		duration 2
		restart 0
		uselight "lightspectrum"
		fadeOut 1.5
	}
	
	{										// rain particles
		delay 1
		name "particle1"
		duration 2.0
		restart 0
		model "spawnSphere_2.prt"
		offset 0, 0, 0
		fadeOut 2
	}
	{										// upper explosion
		delay 2.7
		name "particle2"
		duration 2
		restart 0
		rotate 0.1
		model "spawnSphere_2a.prt"
		offset 0, 0, 0
		fadeOut 2
	}		   
	{										
		name "shake1"
		delay 2.7
		duration 0.5
		restart 0
		shake	125, 2.5, 256, 0, 20000
		offset	-1, 0, 0
	}
	{
		delay 2.7
		name "light5"
		duration 0.05
		restart 0
		light "lights/squareblast", 0.2, 0.02, 0.01, 300
	}
	{										// fade out the blast light
		delay 2.8
		name "light6"
		duration 0.10
		restart 0
		useLight "light5"
		fadeOut 0.005
	}
}

fx fx/teleporter3_fly
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
		duration 1
		restart 0
		light "lights/spectrumlight", 0.2, 0.02, 0.01, 500
		fadeIn 1
	}
	{
		delay 3
		name "lightspectrumoff"
		duration 2
		restart 0
		uselight "lightspectrum"
		fadeOut 1.5
	}
	
	{										// rain particles
		delay 2
		name "particle1"
		duration 2.0
		restart 0
		model "spawnSphere_2.prt"
		offset 0, 0, 0
		fadeOut 2
	}
	{										// upper explosion
		delay 3.7
		name "particle2"
		duration 2
		restart 0
		rotate 0.1
		model "spawnSphere_2a.prt"
		offset 0, 0, 0
		fadeOut 2
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
	{										// fade out the blast light
		delay 3.8
		name "light6"
		duration 0.10
		restart 0
		useLight "light5"
		fadeOut 0.005
	}
}

fx fx/teleporter_fly
{

	{										// sound
		name "sound1"
		delay 0
		sound "teleport_full"		// calls 2-second long effect, sub '_1s' or '_3s' for 1- or 3-second lengths
		duration 17
	}
	
	{
		delay 0
		name "lightspectrum"
		duration 1
		restart 0
		light "lights/spectrumlight", 0.2, 0.02, 0.01, 500
		fadeIn 1
	}
	{
		delay 1
		name "lightspectrumoff"
		duration 2
		restart 0
		uselight "lightspectrum"
		fadeOut 1.5
	}
	
	{										// rain particles
		delay 0
		name "particle1"
		duration 2.0
		restart 0
		model "spawnSphere_2c.prt"
		offset 0, 0, 0
		fadeOut 2
	}
	{										// upper explosion
		delay 0.7
		name "particle2"
		duration 2
		restart 0
		rotate 0.1
		model "spawnSphere_2a.prt"
		offset 0, 0, 0
		fadeOut 2
	}		   
	{										
		name "shake1"
		delay 0.7
		duration 0.5
		restart 0
		shake	125, 2.5, 256, 0, 20000
		offset	-1, 0, 0
	}
	{
		delay 0.7
		name "light5"
		duration 0.05
		restart 0
		light "lights/squareblast", 0.2, 0.02, 0.01, 300
	}
	{										// fade out the blast light
		delay 0.8
		name "light6"
		duration 0.10
		restart 0
		useLight "light5"
		fadeOut 0.005
	}
}