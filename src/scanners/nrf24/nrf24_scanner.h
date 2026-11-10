#pragma once

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

// Analyseur de spectre 2,4 GHz avec 1 ou 2 modules nRF24L01+ (PA+LNA ou non).
//
// Principe : le nRF24L01+ possède un détecteur de puissance reçue (registre RPD) d'UN SEUL BIT,
// qui indique "plus de -64 dBm environ sur le canal réglé" (seuil mesuré à l'entrée de la puce :
// avec un module PA+LNA, le seuil équivalent à l'antenne est plus bas, sans être calibré).
// On balaie les canaux en boucle et on compte, pour chacun, la proportion de balayages
// où le détecteur s'est déclenché : c'est un TAUX D'OCCUPATION, pas une puissance.
//
// Les modules ne font que RECEVOIR (jamais d'émission, auto-ACK désactivé).
// Ils ne décodent rien : on voit l'activité radio, pas l'identité des appareils.

#define SPEC_CHANNELS   84          // canal i = (2400 + i) MHz, de 2400 à 2483 MHz
#define SPEC_MAX_MODULES 2

// Broches (à adapter à votre câblage). Les modules sont sur le bus HSPI, séparé de l'écran (VSPI).
#define NRF_PIN_SCK   25
#define NRF_PIN_MISO  26
#define NRF_PIN_MOSI  27
#define NRF_PIN_CSN_A 32
#define NRF_PIN_CE_A  33
#define NRF_PIN_CSN_B 16
#define NRF_PIN_CE_B  17

// Débit radio : il règle la largeur du filtre de réception. 250 kbit/s = filtre le plus étroit
// (meilleure séparation entre canaux voisins), c'est le réglage de l'exemple de référence de la
// bibliothèque RadioHead. Valeurs : 0 = 250 kbit/s, 1 = 1 Mbit/s, 2 = 2 Mbit/s.
#define NRF_DATA_RATE 0

// Temps laissé au récepteur pour se stabiliser sur un nouveau canal (microsecondes).
// Les exemples de référence utilisent 128 µs (RF24) et "au moins 170 µs" (RadioHead).
#define NRF_SETTLE_US 170

// 1 : écrit sur la liaison série, une fois par seconde, l'état des modules (registres relus) et le nombre
// de détections. Utile pour la mise au point du câblage ; mettre 0 ensuite.
#define NRF_DEBUG 1

// Résultat envoyé à l'application (copie par valeur dans la queue).
struct SpectrumFrame {
    uint8_t  level[SPEC_CHANNELS];   // 0 à 100 : % des balayages de la fenêtre où le canal était occupé
    uint16_t sweepsPerSec;           // cadence de balayage mesurée
    uint8_t  modules;                // nombre de modules en service
};

class SpectrumScanner {
public:
    // Détecte les modules branchés (écriture/relecture d'un registre) et crée la tâche de balayage.
    // frameQueue : queue de longueur 1 ; les trames anciennes sont écrasées (xQueueOverwrite).
    // Retourne le nombre de modules détectés (0, 1 ou 2).
    uint8_t begin(QueueHandle_t frameQueue);

    void start();            // démarre le balayage (non bloquant)
    void stop();             // l'arrête et met les modules en veille (non bloquant)
    bool running();          // état demandé : true entre start() et stop()
    uint8_t modules();
};
