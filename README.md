
# STM32 Pulse Generator ⏱️⚡  

A lightweight and non-blocking C library for pulse generation on STM32 microcontrollers.  
**PulseGen** uses **timers in Output Compare (OC) mode** to generate precise pulse trains without blocking the CPU.  


It can be used for **stepper motor control**, **signal modulation**, or any application that requires periodic pulses with accurate timing.



## 🚀 Features  
- Pulse train generation with **configurable and precise frequency**, available in two modes:  
  - **Finite mode** → Generates a user-specified number of pulses.  
  - **Continuous mode** → Generates pulses continuously until manually stopped.  
- **Non-blocking implementation** using timer interrupts.  
- Fully compatible with **STM32 HAL**.  
- Supports **multiple independent pulse generators** operating simultaneously.



## 📂 Adding the Library to STM32CubeIDE Project

1. Copy the [`Pulse_Generator`](./Pulse_Generator) folder into the `Drivers` directory of your STM32CubeIDE project.

2. In STM32CubeIDE, open your project **Properties → C/C++ General → Paths and Symbols** and add the following include path:
	```text
	/${ProjName}/Drivers/Pulse_Generator/Inc
	```

> Alternatively, you can copy `pulse_gen.h` and `pulse_gen.c` directly into your project source and include directories as needed.


## ⚙️ How to Use  

The basic workflow for using the library is as follows:

1. **Configure timers in STM32CubeIDE**  
	  - Set up the microcontroller clocks as needed, particularly the **APB1** and **APB2** timers, which will drive the timers used for pulse generation.  
		  > **Note:** The maximum achievable pulse frequency is limited by the timer main clock, with an upper bound of *TIMx_CLK / 2*.
		  
	  - Select the timers that will be used for pulse generation.  
	  - For each timer, configure the desired **channels** in **Output Compare mode**, and set the **Toggle on match** mode so the pin automatically changes state on each match.  
	  - In the **NVIC Settings** tab, enable the timer’s global interrupts (`TIMx global interrupt`), which are required for the library to handle events without blocking the CPU.  
	  - Once clocks and timers are configured, generate the code automatically through the IDE.  																												

2. **Library implementation**
	  - Initialize the Step Generator handle. The required parameters are:  
	     - **htim** → The base timer handle on which the generator depends.  
		 - **TimChannel** → The specific channel used to output pulses.  
		 - **MaxPulseFreq** → Maximum pulse frequency allowed (exceeding it triggers an error).  
	     - **MinDeltaCCR** → Defines the timer resolution at the maximum frequency.
        
		   > **Note:** Increasing the maximum frequency or decreasing the minimum CCR increment expands the available frequency range, but reduces the resolution. For a clearer understanding of how these configuration values affect the output, you can refer to the [`PulseGen.xlsx`](./Pulsegen.xlsx) spreadsheet.
      
		   > **Important:** For step generators sharing the same timer, `MaxPulseFreq` and `MinDeltaCCR` must have identical values.

     - Add the callback function `HAL_TIM_OC_DelayElapsedCallback`.
	   The examples provide callback functions for:  
   	     - **Single generator mode**: [`callbacks.c`](./Examples/PulseGenerator_Single/Core/Src/callbacks.c)  
   	     - **Multiple generators mode**: [`callbacks.c`](./Examples/PulseGenerator_Multiple/Core/Src/callbacks.c)
 


## 📊 Usage Examples  
The [`examples/`](./Examples) folder contains ready-to-use reference projects. These examples have been carried out using the STM32F407 Discovery1 development board, and they make use of the Timer 4 channels connected to the board's LEDs, allowing the pulse outputs to be visually observed.
- [PulseGenerator_Single](./Examples/PulseGenerator_Single):  Configures a single pulse generator in finite pulse generation mode.  
Blocks execution until the generation is complete before starting another with a different frequency.
```c
/* Initialize the Pulse Generator application */
APP_PulseGen_Init();
/* Configure pulse frequency to 5 Hz and generate 10 pulses */
PulseGen_SetPulseFreq(&hPulseGen1, 5);
PulseGen_GeneratePulses(&hPulseGen1, 10);
/* Block execution until pulse generation is complete */
while(hPulseGen1.State != PULSEGEN_INACTIVE) {}
/* Reconfigure pulse frequency to 15 Hz and generate 40 pulses */
PulseGen_SetPulseFreq(&hPulseGen1, 15);
PulseGen_GeneratePulses(&hPulseGen1, 40);
```
- [PulseGenerator_Multiple](./Examples/PulseGenerator_Multiple):  Configures multiple pulse generators with different frequencies and starts some in finite pulse generation mode and others in continuous mode.
```c
/* Initialize the Pulse Generator application */
APP_PulseGen_Init();
/* Configure the pulse frequency of each generator */
PulseGen_SetPulseFreq(&hPulseGen1, 5);
PulseGen_SetPulseFreq(&hPulseGen2, 15);
PulseGen_SetPulseFreq(&hPulseGen3, 1);
PulseGen_SetPulseFreq(&hPulseGen4, 3);
/* Start two generators in finite pulse mode and two in continuous mode */
PulseGen_GeneratePulses(&hPulseGen1, 10); // Generate 10 pulses at 5 Hz
PulseGen_GeneratePulses(&hPulseGen2, 30); // Generate 30 pulses at 15 Hz
PulseGen_Start(&hPulseGen3); // Continuous mode at 1 Hz
PulseGen_Start(&hPulseGen4); // Continuous mode at 3 Hz
/* Keep continuous generation active for 10 seconds, then stop */
HAL_Delay(10000);
PulseGen_Stop(&hPulseGen3);
PulseGen_Stop(&hPulseGen4);
```


## 📖 Documentation  
Each function is documented. For further information, please check [`pulse_gen.h`](./Pulse_Generator/Inc/pulse_gen.h) and [`pulse_gen.c`](./Pulse_Generator/Src/pulse_gen.c) for the full API.  

## 🤝 Contributions  
Contributions are welcome. If you find a bug or want to suggest an improvement, open an *issue* or submit a *pull request*.
