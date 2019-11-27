/*
 * project.c
 *
 * Main file
 *
 * Author: Peter Sutton. Modified by Alex Patapan
 */ 

#include <avr/io.h>
#include <avr/interrupt.h>
#include <avr/pgmspace.h>
#include <stdio.h>
#include <stdlib.h>

#include "ledmatrix.h"
#include "scrolling_char_display.h"
#include "buttons.h"
#include "serialio.h"
#include "terminalio.h"
#include "score.h"
#include "timer0.h"
#include "game.h"
#include "pixel_colour.h"

#define F_CPU 8000000L
#include <util/delay.h>

// Function prototypes - these are defined below (after main()) in the order
// given here
void initialise_hardware(void);
void splash_screen(void);
void new_game(void);
void play_game(void);
void handle_game_over(void);
void seven_segment_ports(void);

void update_terminal(void);
void advance_asteroids(void);
void play_sound(uint16_t);
void handle_basehit_sound(void);
void handle_asteroid_animation(int x, int y);
void enable_asteroid_animation(int x, int y);
void redraw_base(uint8_t colour);
void redraw_all_asteroids(void);
void redraw_all_projectiles(void);
void explosion(int x, int y, PixelColour colour);


// ASCII code for Escape character
#define ESCAPE_CHAR 27

//global vars
uint32_t start_shoot_time, start_hit_time, asteroid_animation_time;
int startup, sequence, base_hit_sound;

int basehit_sequence;
uint32_t basehit_time;

int animation_x;
int animation_y;
int asteroid_animation_on;
int asteroid_sequence;

/////////////////////////////// main //////////////////////////////////
int main(void) {
	// Setup hardware and call backs. This will turn on 
	// interrupts.
	initialise_hardware();
	
	// Show the splash screen message. Returns when display
	// is complete
	splash_screen();
	
	while(1) {
		new_game();
		play_game();
		handle_game_over();
	}
}

void initialise_hardware(void) {
	ledmatrix_setup();
	init_button_interrupts();
	// Setup serial port for 19200 baud communication with no echo
	// of incoming characters
	init_serial_stdio(19200,0);
	
	init_timer0();
	
	
	// Turn on global interrupts
	sei();

	//Setup seven segment display
	seven_segment_ports();
	
	// set up ADC - AVCC reference, right adjust
	ADMUX = (1<<REFS0);
	// turn on ADC
	ADCSRA = (1 << ADEN)|(1<<ADPS2)|(1<<ADPS1);
	
	//set PIN D3 output
	DDRD |= (0<<3);
	
}

void seven_segment_ports(void) {
	// Make all bits of port C to be output bits
	DDRC = 0xFF; 
}

// For a given frequency (Hz), return the clock period
uint16_t freq_to_clock_period(uint16_t freq) {
	return (1000000UL / freq);
}

uint16_t duty_cycle_to_pulse_width(float dutycycle, uint16_t clockperiod) {
	return (dutycycle * clockperiod) / 100;
}

void play_sound(uint16_t freq) {
	// set PORT D pins 2, 4 and 5 to be outputs
	DDRD |= (1<<4);
	
	float dutycycle = 50;	// % originally 2
	uint16_t clockperiod = freq_to_clock_period(freq);
	uint16_t pulsewidth = duty_cycle_to_pulse_width(dutycycle, clockperiod);
	
	// Set the maximum count value for timer/counter 1 to be one less than the clockperiod
	OCR1A = clockperiod - 1;
	
	// Set the count compare value based on the pulse width. The value will be 1 less
	// than the pulse width - unless the pulse width is 0.
	if(pulsewidth == 0) {
		OCR1B = 0;
	} else {
		OCR1B = pulsewidth - 1;
	}
	
	// setup timer 1 
	// Fast PWM, reset to 0 on OCR1A. Count at 1MHz (CLK/8)
	// OC1B clears on  compare match, set on timer overflow (non-inverting)
	TCCR1A = (1 << COM1B1) | (0 <<COM1B0) | (1 <<WGM11) | (1 << WGM10);
	TCCR1B = (1 << WGM13) | (1 << WGM12) | (0 << CS12) | (1 << CS11) | (0 << CS10);
	
}

void shoot_sound(void) {
	
	if(PIND & (1 << 3)) {
		play_sound(3000);
		start_shoot_time = get_current_time();
	}
}

void hit_sound(void) {
	
	if(PIND & (1 << 3)) {
		play_sound(1200);
		start_shoot_time = get_current_time();
	}
}

void splash_screen(void) {
	// Clear terminal screen and output a message
	clear_terminal();
	move_cursor(10,10);
	printf_P(PSTR("Asteroids"));
	move_cursor(10,12);
	printf_P(PSTR("CSSE2010/7201 project by Alex Patapan (s44792925)"));
	
	// Output the scrolling message to the LED matrix
	// and wait for a push button to be pushed.
	ledmatrix_clear();
	while(1) {
		set_scrolling_display_text("ASTEROIDS S44792925", COLOUR_GREEN);
		// Scroll the message until it has scrolled off the 
		// display or a button is pushed
		while(scroll_display()) {
			_delay_ms(150);
			if(button_pushed() != NO_BUTTON_PUSHED) {
				return;
			}
		}
	}
}

void new_game(void) {
	
	// Initialise the game and display
	initialise_game();
	
	// Clear the serial terminal
	clear_terminal();
	
	// Initialise the score
	init_score();
	
	//setup terminal
	update_terminal();
	
	// Clear a button push or serial input if any are waiting
	// (The cast to void means the return value is ignored.)
	(void)button_pushed();
	clear_serial_input_buffer();
	
	basehit_sequence=1;
	basehit_time = get_current_time();
	
	animation_x = 0;
	animation_y = 0;
	asteroid_animation_on=0;
	asteroid_sequence = 1;
}

int pause = 0;

void play_game(void) {
	uint32_t current_time, last_move_time, last_asteroid_time, last_base_move;
	int8_t button;
	char serial_input, escape_sequence_char;
	uint8_t characters_into_escape_sequence = 0;
	
	uint16_t value;
	uint8_t axis = 0;
	
	startup = 1;
	sequence = 1;
	
	// Get the current time and remember this as the last time the projectiles
    // were moved.
	current_time = get_current_time();
	last_move_time = current_time;
	last_asteroid_time = current_time;
	last_base_move = current_time;
	uint32_t startup_sequence = current_time;
	
	if ((PIND & (1 << 3))) {
		play_sound(500);	
	} else {
		startup = 0;
	}
	
	// We play the game until it's over
	while(!is_game_over()) {
		
		if (!(PIND & (1 << 3))) {
			DDRD &= (0<<4);
		}
		
		
		//make startup noises
		if (startup) {		
			
			if((get_current_time()-(250-200*sin(sequence/1.1)) >= startup_sequence) && (PIND & (1 << 3))) {
				
				switch (sequence) {
					case 1:
						play_sound(800);
						break;
					case 2:
						play_sound(1500);
						break;
					case 3:
						play_sound(2000);
						break;
					case 4:
						play_sound(2500);
						break;
				}
				sequence++;
				startup_sequence = get_current_time();
			}
			
			if ((sequence == 5) || !(PIND & (1 << 3))) {
				startup = 0;
			}
		}
		
		
		
		// Check for input - which could be a button push or serial input.
		// Serial input may be part of an escape sequence, e.g. ESC [ D
		// is a left cursor key press. At most one of the following three
		// variables will be set to a value other than -1 if input is available.
		// (We don't initalise button to -1 since button_pushed() will return -1
		// if no button pushes are waiting to be returned.)
		// Button pushes take priority over serial input. If there are both then
		// we'll retrieve the serial input the next time through this loop
		serial_input = -1;
		escape_sequence_char = -1;
		button = button_pushed();
		
		if(button == NO_BUTTON_PUSHED) {
			// No push button was pushed, see if there is any serial input
			if(serial_input_available()) {
				// Serial data was available - read the data from standard input
				serial_input = fgetc(stdin);
				// Check if the character is part of an escape sequence
				if(characters_into_escape_sequence == 0 && serial_input == ESCAPE_CHAR) {
					// We've hit the first character in an escape sequence (escape)
					characters_into_escape_sequence++;
					serial_input = -1; // Don't further process this character
				} else if(characters_into_escape_sequence == 1 && serial_input == '[') {
					// We've hit the second character in an escape sequence
					characters_into_escape_sequence++;
					serial_input = -1; // Don't further process this character
				} else if(characters_into_escape_sequence == 2) {
					// Third (and last) character in the escape sequence
					escape_sequence_char = serial_input;
					serial_input = -1;  // Don't further process this character - we
										// deal with it as part of the escape sequence
					characters_into_escape_sequence = 0;
				} else {
					// Character was not part of an escape sequence (or we received
					// an invalid second character in the sequence). We'll process 
					// the data in the serial_input variable.
					characters_into_escape_sequence = 0;
				}
			}
		}
		
		// Process the input. 
		if(button==3 || escape_sequence_char=='D' || serial_input=='L' || serial_input=='l') {
			// Button 3 pressed OR left cursor key escape sequence completed OR
			// letter L (lowercase or uppercase) pressed - attempt to move left
			move_base(MOVE_LEFT);
		} else if(button==2 || escape_sequence_char=='A' || serial_input==' ') {
			// Button 2 pressed or up cursor key escape sequence completed OR
			// space bar pressed - attempt to fire projectile
			fire_projectile();
		} else if(button==1 || escape_sequence_char=='B') {
			// Button 1 pressed OR down cursor key escape sequence completed
			// Ignore at present
		} else if(button==0 || escape_sequence_char=='C' || serial_input=='R' || serial_input=='r') {
			// Button 0 pressed OR right cursor key escape sequence completed OR
			// letter R (lowercase or uppercase) pressed - attempt to move right
			move_base(MOVE_RIGHT);
		} else if(serial_input == 'p' || serial_input == 'P') {
			// Unimplemented feature - pause/unpause the game until 'p' or 'P' is
			// pressed again
			serial_input = -1;
			
			
			// stop timers
			TCCR0B &= 0B11111000;
			DDRD &= (0<<4);
			
			while (serial_input != 'p' && serial_input != 'P') {
				if(serial_input_available()) {
					// Serial data was available - read the data from standard input
					serial_input = fgetc(stdin);
				}
			}
			// start timers
			TCCR0B = (1<<CS01)|(1<<CS00);
			
		}
		// else - invalid input or we're part way through an escape sequence -
		// do nothing
		
		current_time = get_current_time();
		
		// accelerate asteroids
		if(!is_game_over() && (current_time >= last_asteroid_time + 500-get_score()*1.8)) {
			// 500ms (0.5 second) has passed since the last time we moved
			// the projectiles - move them - and keep track of the time we
			// moved them
			
			advance_asteroids();
			last_asteroid_time = current_time;
		}
				
		if(!is_game_over() && current_time >= last_move_time + 500) {
			// 500ms (0.5 second) has passed since the last time we moved
			// the projectiles - move them - and keep track of the time we 
			// moved them
			
			advance_projectiles();
			last_move_time = current_time;
		}
		
		// joystick controls
		if(!is_game_over() && current_time >= last_base_move + 50) {
			// Set the ADC mux to choose ADC0 if x_or_y is 0, ADC1 if x_or_y is 1
			if(axis == 0) {
				ADMUX &= ~1;
				} else {
				ADMUX |= 1;
			}
			// Start the ADC conversion
			ADCSRA |= (1<<ADSC);
			
			while(ADCSRA & (1<<ADSC)) {
				; /* Wait until conversion finished */
			}
			value = ADC; // read the value
			if(axis == 0) {
				// X value - move base
				if (value > 700) {
					move_base(MOVE_LEFT);
					} else if (value < 300) {
					move_base(MOVE_RIGHT);
				}
				} else {
				//Y value - shoot
				if (value > 700 || value < 300) {
					fire_projectile();
				}
			}
			// Next time through the loop, do the other direction
			axis ^= 1;
			last_base_move = current_time;	
		}
		
		//handle basehit sound
		if (!is_game_over() && base_hit_sound){
			handle_basehit_sound();	
		}
		
		//handle asteroid animation
		if (!is_game_over() && asteroid_animation_on){
			handle_asteroid_animation(animation_x, animation_y);
		}
		
		
		// stop shooting sound
		if (!base_hit_sound && !startup && (start_shoot_time <= current_time-100)) {
			DDRD &= (0<<4);
		}
	
	}
	
	// We get here if the game is over.
	
}

void handle_basehit_sound(void) {
	if((get_current_time()-(300-200*sin(basehit_sequence/0.8)) >= basehit_time)) {
		if ((PIND & (1 << 3))) {
			switch (basehit_sequence) {
				case 1:
				play_sound(500);
				break;
				case 2:
				play_sound(350);
				break;
				case 3:
				play_sound(200);
				break;
			}	
		}
		
		basehit_sequence++;
		basehit_time = get_current_time();
	}
	
	if (basehit_sequence == 4) {
		base_hit_sound = 0;
	}
	
}

void enable_basehit_sound(void) {
	base_hit_sound = 1;
	basehit_sequence = 1;
}

void handle_asteroid_animation(int x, int y) {
	
	if (get_current_time()-10 >= asteroid_animation_time) {
		switch (asteroid_sequence) {
			case 1: 
				explosion(x, y, COLOUR_ORANGE);
			break;
			case 2: 
				explosion(x, y, COLOUR_LIGHT_ORANGE);
			break;
			case 3: 
				explosion(x, y, COLOUR_ORANGE);
			break;
			case 4: 
				explosion(x, y, COLOUR_BLACK);
			asteroid_animation_on = 0;
		}
		asteroid_animation_time = get_current_time();
		asteroid_sequence++;
		redraw_all_projectiles();
		redraw_all_asteroids();
		
	}
}

void explosion(int x, int y, PixelColour colour) {
	ledmatrix_update_pixel(y,7-x, colour);
	ledmatrix_update_pixel(y,7-x+1, colour);
	ledmatrix_update_pixel(y,7-x-1, colour);
	ledmatrix_update_pixel(y+1,7-x, colour);
	if (y-1 >= 3) {
		ledmatrix_update_pixel(y-1,7-x, colour);
	}
		
	
	
	
}

void enable_asteroid_animation(int x, int y) {
	
	if (!asteroid_animation_on) {
		asteroid_animation_on = 1;
		asteroid_sequence = 1;
		asteroid_animation_time = get_current_time();
		
		animation_x = x;
		animation_y = y;
	}
	
}

void handle_game_over() {
	
	move_cursor(10,13);
	clear_to_end_of_line();
	move_cursor(10,13);
	printf_P(PSTR("Lives: 0"));
	move_cursor(10,14);
	printf_P(PSTR("GAME OVER"));
	move_cursor(10,15);
	printf_P(PSTR("Press a button to start again"));
	
	base_hit_sound = 1;
	basehit_sequence = 1;

	int gameover_sequence = 0;
	int arrangement = 0;
	int reset = 0;
	PixelColour colour[4] = {COLOUR_YELLOW, COLOUR_ORANGE}; 
	PixelColour colour2[4] = {COLOUR_RED, COLOUR_GREEN}; 	
	PixelColour pixelcolour;
	
	while(button_pushed() == NO_BUTTON_PUSHED) {
		; // wait
		
		//handle basehit sound
		if (base_hit_sound){
			handle_basehit_sound();
		} else {
			DDRD &= (0<<4);
		}
		
		_delay_ms(100);
		// gameover animation
		if (gameover_sequence<64) {
			for (int i=0; i<8; i++) {  
				//colours 1 row 
				if (((16 <= gameover_sequence) && (gameover_sequence < 32)) || ((48 <= gameover_sequence) && (gameover_sequence < 64))) {
					pixelcolour = COLOUR_BLACK;
				} else if (gameover_sequence>=32) {
					pixelcolour = colour[(i+arrangement)%2];
				} else {
					pixelcolour = colour2[(i+arrangement)%2];
				}
				ledmatrix_update_pixel(gameover_sequence%16, i, pixelcolour);
			}
			gameover_sequence++;
			arrangement++;
			
		} else {
			gameover_sequence = 0;
			reset = ~reset;
		}
	}
	
}
