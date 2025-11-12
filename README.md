# hivernacleIOT

1. Descripció del Projecte

El mercat del cultiu d'interior i la jardineria urbana automatitzada està creixent ràpidament. Tanmateix, les solucions comercials que dominen actualment, com les de "Click & Grow", solen ser ecosistemes tancats amb preus elevats (sovint superant els 800-900€), la qual cosa en limita la personalització i l'accessibilitat.

La meva proposta és dissenyar i implementar un prototip funcional d'hivernacle intel·ligent (o "indoor garden") enfocat a planter i cultiu domèstic de petita escala. El repte és aconseguir un sistema amb un cost significativament més reduït i una arquitectura de hardware i software oberta i personalitzable, com a alternativa a les opcions comercials.

El nucli del TFG serà el desenvolupament d'un sistema encastat (embedded) capaç de gestionar de forma autònoma les condicions clau del cultiu (llum, reg, temperatura i humitat) i, alhora, oferir connectivitat per a la seva monitorització i control remot.

Perquè el projecte tingui una orientació pràctica i propera a un producte real, em centraré en diversos aspectes clau:

El disseny de hardware propi (una PCB personalitzada) per optimitzar costos i el factor de forma, anant més enllà de l'ús de mòduls de desenvolupament estàndard.

L'ús d'un Sistema Operatiu en Temps Real (RTOS) per garantir una execució robusta i gestionar eficientment les tasques concurrents (lectura de sensors, control d'actuadors, comunicacions).

La implementació d'un sistema d'actualitzacions de firmware Over-The-Air (OTA), essencial per al manteniment i la millora contínua del producte.

Una anàlisi de l'arquitectura de servei, comparant un model de servidor local (prioritzant la privadesa de l'usuari) amb un servidor extern al núvol (que permetria accés remot universal i obriria la porta a models de subscripció).

L'estudi de la connectivitat més enllà del Wi-Fi, explorant la viabilitat d'integrar mòduls 5G per a aplicacions de major escala o per a ubicacions sense una xarxa Wi-Fi fiable.

2. Objectius del Projecte

Objectiu General

Dissenyar, implementar i validar un prototip complet i funcional d'hivernacle intel·ligent IoT, demostrant la viabilitat d'una solució de baix cost, oberta i amb característiques pròpies d'un producte comercial.

Objectius Específics

Anàlisi i Disseny:

Realitzar un estudi de l'estat de l'art, analitzant productes similars (com Click & Grow) i solucions IoT existents.

Definir amb precisió els requisits funcionals i tècnics del sistema.

Dissenyar l'arquitectura hardware i software completa.

Desenvolupament de Hardware:

Seleccionar els sensors (temperatura, humitat, llum) i actuadors (bombes de reg, LEDs de cultiu, ventiladors/calefactors) més adequats per al projecte.

Dissenyar i fabricar una placa de circuit imprès (PCB) personalitzada que integri el microcontrolador, l'etapa de potència per als actuadors i els connectors dels sensors.

Desenvolupament de Software Encastat (Firmware):

La base del firmware serà un Sistema Operatiu en Temps Real (RTOS) (com ara FreeRTOS) per gestionar la concurrència de tasques.

Implementar els controladors (drivers) per a tots els perifèrics (sensors i actuadors).

Desenvolupar els algorismes d'automatització, incloent un controlador PID per a la gestió de la temperatura i lògiques de reg i il·luminació basades en paràmetres d'usuari i lectures dels sensors.

Connectivitat i Serveis:

Implementar la connectivitat Wi-Fi, incloent un procés de configuració inicial senzill per a l'usuari (tipus hotspot temporal).

Integrar un mecanisme fiable d'actualitzacions OTA per al firmware.

Avaluar i implementar una de les dues arquitectures de servidor proposades:

Opció A (Local): Un servidor web allotjat al propi dispositiu per a control dins de la xarxa domèstica.

Opció B (Cloud): Comunicació amb un servidor extern (núvol) per a accés remot, gestió d'usuaris i emmagatzematge d'historials.

Implementar un sistema d'alertes a l'usuari (p. ex., via servidor de correu) per notificar valors anòmals o errors del sistema.

Estudiar a nivell teòric la integració i els costos associats a mòduls de connectivitat cel·lular (5G/LTE-M).

Interfície d'Usuari:

Desenvolupar una interfície d'usuari bàsica (ja sigui web o una PWA mòbil) per a la visualització de dades en temps real, la configuració dels paràmetres de cultiu i la consulta d'alertes.

Validació i Proves:

Integrar tots els components desenvolupats (hardware, firmware, backend, frontend).

Realitzar un pla de proves funcionals completes per validar que el prototip compleix tots els requisits definits.

3. Metodologia i Pla de Treball

El projecte s'afrontarà amb una metodologia de desenvolupament iterativa, dividida en les fases següents:

Fase 1: Anàlisi i Disseny: Estat de l'art, definició de requisits, selecció de tecnologies i disseny d'arquitectura.

Fase 2: Prototipat Hardware: Proves inicials amb mòduls (DevKits), disseny de PCB, fabricació i validació de la placa.

Fase 3: Desenvolupament Firmware: Implementació de RTOS, drivers, algorismes de control (PID) i lògiques d'automatització.

Fase 4: Connectivitat i Backend: Implementació de Wi-Fi, OTA, i desenvolupament del servidor (local o cloud) i la seva API.

Fase 5: Interfície i Integració: Desenvolupament de la UI, integració completa del sistema i proves funcionals.

Fase 6: Documentació i Validació Final: Redacció de la memòria del TFG i execució de les proves de validació finals.

4. Tecnologies Clau a Utilitzar

Hardware: Microcontrolador tipus ESP32 (per la seva potència, Wi-Fi integrat i suport per RTOS), sensors (DHT22, fotoresistències, sensors d'humitat de sòl capacitius), actuadors (relés, MOSFETs, bombes d'aigua), disseny de PCB (KiCad/Eagle).

Software Encastat: C/C++, FreeRTOS.

Comunicacions: Wi-Fi, MQTT (com a protocol IoT estàndard), HTTP/REST (per a l'API), protocols OTA.

Backend (en cas d'optar per núvol): Python (Flask/Django) o Node.js, Base de dades (PostgreSQL/InfluxDB per a sèries temporals), Cloud (AWS, Firebase o un Cloud privat).

Frontend: HTML/CSS/JavaScript (possiblement amb un framework lleuger com React o Vue si el temps ho permet).

5. Resultats Esperats

Al finalitzar el TFG, s'entregaran els següents resultats:

Un prototip funcional i integrat de l'hivernacle intel·ligent.

Tota la documentació de disseny de hardware (esquemes i fitxers PCB) de la placa controladora.

El codi font complet del firmware, el backend i el frontend, degudament comentat i estructurat.

Una anàlisi comparativa documentada sobre les arquitectures de servidor (local vs. cloud) i connectivitat (Wi-Fi vs. 5G).

La memòria final del TFG, que detallarà tot el procés de disseny, implementació i els resultats obtinguts.
