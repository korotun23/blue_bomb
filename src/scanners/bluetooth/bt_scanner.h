#pragma once

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

// Scanner Bluetooth double mode : BLE puis Bluetooth classique (BR/EDR).
// Pile Bluedroid (la seule qui gère le classique) : remplace NimBLE-Arduino.

enum BtDeviceType : uint8_t {
    BT_TYPE_BLE     = 0,
    BT_TYPE_CLASSIC = 1
};

#define BT_MAX_SERVICES 4

// Copiée par valeur dans la queue FreeRTOS : pas de String ni de pointeur.
// Un même périphérique peut produire plusieurs messages (annonce, puis scan
// response ; ou résultat d'inquiry, puis nom). C'est à l'application de les fusionner.
struct BtDeviceInfo {
    char     name[32];      // vide si inconnu
    char     address[18];   // "aa:bb:cc:dd:ee:ff"
    uint8_t  type;          // BtDeviceType
    bool     hasRssi;
    int8_t   rssi;
    bool     hasCompany;    // identifiant constructeur lu dans les données constructeur
    uint16_t companyId;
    uint32_t cod;           // Classic : Class of Device (0 si absent)

    // BLE uniquement
    uint8_t  addrType;      // 0 publique, 1 aléatoire, 2 et 3 : adresses privées résolvables
    bool     hasTxPower;
    int8_t   txPower;       // dBm, si annoncée
    uint8_t  connectable;   // 0 inconnu, 1 oui, 2 non
    uint8_t  svcCount;      // UUID de services 16 bits annoncés (BT_MAX_SERVICES au plus)
    bool     svcMore;       // true s'il y en avait davantage
    uint16_t svc[BT_MAX_SERVICES];
};

// Ajoute un UUID 16 bits sans doublon
inline void btAddService(BtDeviceInfo &d, uint16_t uuid)
{
    for (uint8_t i = 0; i < d.svcCount; i++) {
        if (d.svc[i] == uuid) return;
    }
    if (d.svcCount < BT_MAX_SERVICES) d.svc[d.svcCount++] = uuid;
    else                              d.svcMore = true;
}

class BtScanner {
public:
    // Initialise le contrôleur et Bluedroid. false en cas d'échec (ex. manque de RAM).
    bool begin(QueueHandle_t queue);

    // Un cycle = scan BLE de bleMs millisecondes, puis inquiry classique de
    // classicUnits x 1,28 s (1 à 48). Mettre 0 à l'une des deux valeurs la désactive.
    // repeat = false : un seul cycle. repeat = true : les cycles s'enchaînent jusqu'à stopScan().
    void startScan(uint32_t bleMs, uint8_t classicUnits, bool repeat = false);

    // Demande l'arrêt. Il n'est effectif que lorsque isScanning() repasse à false.
    void stopScan();

    bool isScanning();      // true tant que la session n'est pas terminée
    bool isStopping();      // true entre stopScan() et la fin effective
    uint16_t cycle();       // numéro du cycle en cours (1 = premier), 0 si aucun scan lancé
    uint32_t droppedCount();// messages perdus car la queue était pleine (depuis le démarrage)
};
