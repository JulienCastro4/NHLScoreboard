# NHL Scoreboard - ESP32 Hub75 Display

Tableau d'affichage NHL en temps réel utilisant un ESP32-S3 et un panneau LED matriciel Hub75 32x64. Affiche les scores en direct, les équipes, et des animations de but.

## 🏒 Fonctionnalités

- **Scores en direct** : Récupération des matchs NHL via l'API NHL
- **Matchs internationaux** : Support des équipes nationales (Jeux Olympiques, Coupes du Monde)
- **Affichage dynamique** : Panneau Hub75 32x64 avec logos d'équipes 20x20
- **Animations de but** : Scène animée déclenchée lors d'un but
- **API Web** : Interface REST pour contrôler l'affichage
- **Interface Web** : Page HTML pour sélectionner les matchs
- **mDNS** : Accès via `http://scoreboardapp.local`

## 🔧 Matériel requis

- **ESP32-S3** (Freenove ESP32-S3 WROOM)
- **Panneau Hub75** 32x64 RGB LED Matrix
- **Alimentation** 5V pour le panneau LED

## 📦 Installation

### 1. Cloner le projet

```bash
git clone <votre-repo>
cd Scoreboard
```

### 2. Configuration PlatformIO

Ce projet utilise [PlatformIO](https://platformio.org/). Installez-le via VS Code ou CLI :

```bash
pip install platformio
```

### 3. Configuration WiFi

Copiez le template de secrets et ajoutez vos credentials WiFi :

```bash
cp include/secrets.h.template include/secrets.h
```

Éditez `include/secrets.h` :

```cpp
#define WIFI_SSID_SECRET "votre_ssid"
#define WIFI_PASS_SECRET "votre_mot_de_passe"
```

### 4. Upload du système de fichiers

Le dossier `data/` contient les logos d'équipes et l'interface web. Uploadez-le sur le LittleFS :

```bash
pio run --target uploadfs
```

### 5. Build et upload du firmware

```bash
pio run --target upload
```

### 6. Moniteur série

```bash
pio device monitor
```

## 🌐 API Web

Une fois connecté, accédez à l'interface via :

- **mDNS** : `http://scoreboardapp.local`
- **IP directe** : Vérifiez l'IP dans le moniteur série

### Endpoints

| Méthode | Endpoint | Description |
|---------|----------|-------------|
| `GET` | `/schedule` | Liste des matchs du jour |
| `POST` | `/select-game` | Sélectionner un match (JSON: `{"gameId": 123456}`) |
| `GET` | `/current-game` | Match actuellement affiché |
| `POST` | `/display/on` | Activer l'affichage |
| `POST` | `/display/off` | Désactiver l'affichage |
| `POST` | `/preview-goal` | Déclencher l'animation de but (test) |

## 🎨 Structure du projet

```
├── data/                    # Fichiers système (LittleFS)
│   ├── index.html          # Interface web
│   └── logos/              # Logos NHL en RGB565 (20x20)
├── include/                # Headers
│   ├── api_server.h        # Serveur API REST
│   ├── schedule_service.h  # Service récupération matchs
│   ├── playbyplay_service.h # Service play-by-play
│   ├── secrets.h.template  # Template credentials WiFi
│   └── display/            # Système d'affichage
│       ├── display_manager.h
│       ├── scoreboard_scene.h
│       ├── goal_scene.h
│       ├── animator.h
│       └── logo_cache.h
├── src/                    # Code source
│   ├── main.cpp           # Point d'entrée
│   ├── api_server.cpp
│   ├── schedule_service.cpp
│   ├── playbyplay_service.cpp
│   └── display/           # Implémentations affichage
├── tools/
│   └── logo_builder/      # Scripts Python génération logos
└── platformio.ini         # Configuration PlatformIO
```

## 🛠️ Outils

### Logo Builder

Génère les logos NHL et internationaux au format RGB565 pour le panneau Hub75.

**Documentation complète :** [tools/logo_builder/README.md](tools/logo_builder/README.md)

**Logos NHL :**
```bash
cd tools/logo_builder
python -m venv .venv
.venv\Scripts\activate
pip install -r requirements.txt
python build_logos.py
```

**Logos Internationaux (Jeux Olympiques, Coupes du Monde) :**
```bash
cd tools/logo_builder
python build_international_logos.py
```

Les deux scripts copient automatiquement les logos générés vers `data/logos/`.

**Équipes internationales supportées :** CAN, USA, FIN, SWE, CZE, RUS, SVK, SUI, GER, ITA, LAT, DEN, NOR, AUT, FRA

## 🐛 Dépannage

### Le panneau LED ne s'allume pas

- Vérifiez les connexions Hub75
- Vérifiez l'alimentation 5V du panneau
- Consultez `include/display/hub75_pins.h` pour la configuration des pins

### Impossible de se connecter au WiFi

- Vérifiez `include/secrets.h`
- Vérifiez le moniteur série pour les erreurs
- Assurez-vous que le réseau WiFi est en 2.4GHz (pas 5GHz)

### LittleFS n'est pas initialisé

- Uploadez le système de fichiers : `pio run --target uploadfs`
- Assurez-vous que `board_build.filesystem = littlefs` est dans `platformio.ini`

### mDNS ne fonctionne pas

- Accédez directement via l'IP affichée dans le moniteur série
- Sous Windows, installez Bonjour (ou iTunes)
- Sous Linux, installez `avahi-daemon`

## 📚 Dépendances

Les bibliothèques sont gérées automatiquement par PlatformIO :

- **ArduinoJson** (^7.0.0) : Parsing JSON
- **ESP32-HUB75-MatrixPanel-DMA** : Pilote Hub75
- **Adafruit GFX Library** : Primitives graphiques

## 📝 License

Ce projet est fourni tel quel pour usage personnel et éducatif.

## 🤝 Contributions

Les contributions sont les bienvenues ! N'hésitez pas à ouvrir une issue ou une pull request.

---

**Fait avec ❤️ pour les fans de hockey** 🏒
