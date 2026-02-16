# Example using pygame to play a sound file
# First, install pygame: pip install pygame

import pygame

def play_sound(filename):
    pygame.mixer.init()
    pygame.mixer.music.load(filename)
    pygame.mixer.music.play()
    while pygame.mixer.music.get_busy():
        pygame.time.Clock().tick(10)

# Example usage (replace 'sound.wav' with your audio file path):
play_sound('/usr/share/sounds/alsa/Front_Center.wav')
