# Implements src/audio.h (found through orb_shared). Named orb_audio: ESP-IDF projects often
# carry an "audio" component of their own from the ADF, and this is not that.
idf_component_register(SRCS "audio.cpp"
                       REQUIRES orb_shared)
