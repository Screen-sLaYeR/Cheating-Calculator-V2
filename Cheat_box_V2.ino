#include <WiFi.h>
#include <WebServer.h>
#include <LittleFS.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <esp_wifi.h> // Required for advanced power control

/* ---------- Pins ---------- */
#define BTN_UP     8
#define BTN_DOWN   9
#define BTN_ENTER  12

#define I2C_SDA    6
#define I2C_SCL    7

/* ---------- OLED ---------- */
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 32
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);

/* ---------- Web ---------- */
WebServer server(80);
IPAddress apIP(192, 168, 4, 1);     
IPAddress netMsk(255, 255, 255, 0);

/* ---------- State Management ---------- */
enum AppState { MAIN_MENU, FILE_BROWSER, FILE_VIEWER, UPLOAD_MODE };
volatile AppState currentState = MAIN_MENU; 

bool isScreenOff = false;

/* ---------- Navigation Variables ---------- */
int menuIndex = 0;
int listScrollOffset = 0;

String fileList[20];
int fileCount = 0;

// Viewer variables
String currentFileContent = "";
int totalFileLines = 0;
int viewScrollLine = 0;

/* ---------- Button Vars ---------- */
int lastU = HIGH, lastD = HIGH, lastE = HIGH;
unsigned long upPressStart = 0;
bool upHeld = false;
bool ignoreNextUpClick = false;

const unsigned long DEBOUNCE_DELAY = 50; 
const unsigned long LONG_PRESS_TIME = 1500;

/* ---------- Task Handles ---------- */
TaskHandle_t TaskUI;
TaskHandle_t TaskServer;

/* ---------- Prototypes ---------- */
void loadFiles();
void drawList(String items[], int count, int selIndex);
void drawFileViewer();
void drawUploadScreen(String status);
int countLines(String str);
String getLine(String str, int lineIdx);

/* =========================================
   TASK: UI & INPUT (Runs on Core 1)
   ========================================= */
void taskUI_code(void * pvParameters) {
  for(;;) {
    int u = digitalRead(BTN_UP);
    int d = digitalRead(BTN_DOWN);
    int e = digitalRead(BTN_ENTER);

    // Screen Wake
    if (isScreenOff) {
      if (d == LOW || e == LOW) {
        isScreenOff = false;
        display.ssd1306_command(SSD1306_DISPLAYON);
        delay(200); 
        lastU = u; lastD = d; lastE = e;
      }
    } 
    else {
      // UP Long Press (Screen Off)
      if (u == LOW && lastU == HIGH) { upPressStart = millis(); upHeld = true; }
      if (u == LOW && upHeld && (millis() - upPressStart > LONG_PRESS_TIME)) {
          isScreenOff = true;
          display.clearDisplay(); display.display();
          display.ssd1306_command(SSD1306_DISPLAYOFF);
          upHeld = false; ignoreNextUpClick = true; 
      }
      if (u == HIGH && lastU == LOW) upHeld = false;

      // Click Logic
      if (lastU == LOW && u == HIGH && !ignoreNextUpClick) {
         if (currentState == MAIN_MENU && menuIndex > 0) menuIndex--;
         else if (currentState == FILE_BROWSER && menuIndex > 0) menuIndex--;
         else if (currentState == FILE_VIEWER && viewScrollLine > 0) viewScrollLine--;
         ignoreNextUpClick = false; 
      }

      if (lastD == HIGH && d == LOW) {
        delay(DEBOUNCE_DELAY);
        if (digitalRead(BTN_DOWN) == LOW) {
           if (currentState == MAIN_MENU && menuIndex < 1) menuIndex++;
           else if (currentState == FILE_BROWSER && menuIndex < fileCount - 1) menuIndex++;
           else if (currentState == FILE_VIEWER && viewScrollLine < totalFileLines - 3) viewScrollLine++;
        }
      }

      if (lastE == HIGH && e == LOW) {
        delay(DEBOUNCE_DELAY);
        if (digitalRead(BTN_ENTER) == LOW) {
          if (currentState == MAIN_MENU) {
            if (menuIndex == 0) { // Read
               loadFiles(); menuIndex = 0; listScrollOffset = 0; currentState = FILE_BROWSER;
            } else { // Upload
               currentState = UPLOAD_MODE;
            }
          } 
          else if (currentState == FILE_BROWSER) {
            if (menuIndex == 0) { currentState = MAIN_MENU; menuIndex = 0; }
            else {
              File f = LittleFS.open(fileList[menuIndex], "r");
              if(f) {
                currentFileContent = f.readString(); f.close();
                totalFileLines = countLines(currentFileContent); viewScrollLine = 0;
                currentState = FILE_VIEWER;
              }
            }
          }
          else if (currentState == FILE_VIEWER) { currentState = FILE_BROWSER; currentFileContent = ""; }
          else if (currentState == UPLOAD_MODE) { currentState = MAIN_MENU; }
        }
      }
    }
    lastU = u; lastD = d; lastE = e;

    // Draw UI
    if (!isScreenOff) {
      if (currentState == MAIN_MENU) {
        String items[] = {"Read Files", "WiFi Upload"};
        drawList(items, 2, menuIndex);
      } 
      else if (currentState == FILE_BROWSER) drawList(fileList, fileCount, menuIndex);
      else if (currentState == FILE_VIEWER) drawFileViewer();
      // Note: Upload Screen drawing is handled inside the draw function via shared variables
      // But we just trigger it here.
      else if (currentState == UPLOAD_MODE) drawUploadScreen("");
    }
    vTaskDelay(20 / portTICK_PERIOD_MS); 
  }
}

/* =========================================
   TASK: SERVER (Runs on Core 0)
   ========================================= */
void taskServer_code(void * pvParameters) {
  bool wifiActive = false;

  for(;;) {
    if (currentState == UPLOAD_MODE) {
      
      if (!wifiActive) {
        // --- 1. SAFE STARTUP SEQUENCE ---
        
        // Notify User on OLED (Direct draw for status update)
        display.clearDisplay();
        display.setCursor(0,0); display.print("WiFi Starting...");
        display.display();

        WiFi.disconnect(true);
        WiFi.mode(WIFI_OFF);
        vTaskDelay(200 / portTICK_PERIOD_MS); // Let voltage recover

        // Force AP Mode
        WiFi.mode(WIFI_AP);
        
        // --- YOUR FIX: LOWER TX POWER ---
        // We set it after mode selection, but before starting AP
        WiFi.setTxPower(WIFI_POWER_8_5dBm); 
        vTaskDelay(50 / portTICK_PERIOD_MS);

        // Configure IP
        WiFi.softAPConfig(apIP, apIP, netMsk);
        vTaskDelay(50 / portTICK_PERIOD_MS);
        
        // Start AP
        if(WiFi.softAP("ESP_Reader_S3", "12345678")) {
          server.begin();
          wifiActive = true;
          // Notify Success
          display.setCursor(0,10); display.print("Ready!");
          display.display();
          vTaskDelay(500 / portTICK_PERIOD_MS);
        } else {
           display.setCursor(0,10); display.print("AP FAILED");
           display.display();
           vTaskDelay(2000 / portTICK_PERIOD_MS);
           currentState = MAIN_MENU; // Abort
        }
      }

      if (wifiActive) {
        server.handleClient();
        vTaskDelay(5 / portTICK_PERIOD_MS); 
      }

    } else {
      // --- SHUTDOWN SEQUENCE ---
      if (wifiActive) {
        server.stop();
        WiFi.softAPdisconnect(true);
        WiFi.mode(WIFI_OFF);
        wifiActive = false;
      }
      vTaskDelay(200 / portTICK_PERIOD_MS);
    }
  }
}

/* =========================================
   SETUP
   ========================================= */
void setup() {
  Serial.begin(115200);
  
  pinMode(BTN_UP, INPUT_PULLUP);
  pinMode(BTN_DOWN, INPUT_PULLUP);
  pinMode(BTN_ENTER, INPUT_PULLUP);

  Wire.begin(I2C_SDA, I2C_SCL); 

  if(!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
     for(;;); // Loop forever if display fails
  }
  
  display.setRotation(0); 
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.clearDisplay();
  display.println("CheatBox v3");
  display.display();
  delay(500);
  
  if(!LittleFS.begin(true)){
    display.clearDisplay();
    display.println("Formatting FS...");
    display.display();
  }

  // ======================================================
  //              WEB SERVER HANDLERS
  // ======================================================

  // 1. MAIN PAGE (Dark Mode + File List + Upload Form)
  server.on("/", HTTP_GET, [](){
    String html = "<!DOCTYPE html><html><head>";
    html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
    html += "<style>";
    // DARK MODE CSS
    html += "body { font-family: sans-serif; text-align: center; margin: 0; padding: 20px; background-color: #121212; color: #e0e0e0; }";
    html += "h2 { color: #ffffff; margin-bottom: 20px; }";
    
    // Containers (Cards)
    html += ".card { background: #1e1e1e; padding: 20px; border-radius: 12px; display: inline-block; box-shadow: 0 4px 10px rgba(0,0,0,0.5); width: 100%; max-width: 400px; margin-bottom: 20px; text-align: left; }";
    
    // Upload Form
    html += "input[type='file'] { margin-bottom: 15px; color: #bbb; width: 100%; }";
    html += "input[type='submit'] { background-color: #03dac6; color: #000; border: none; padding: 12px; border-radius: 6px; cursor: pointer; width: 100%; font-weight: bold; }";
    
    // File List Styling
    html += "ul { list-style: none; padding: 0; margin: 0; }";
    html += "li { background: #2c2c2c; padding: 10px; border-bottom: 1px solid #333; display: flex; justify-content: space-between; align-items: center; }";
    html += "li:first-child { border-top-left-radius: 8px; border-top-right-radius: 8px; }";
    html += "li:last-child { border-bottom-left-radius: 8px; border-bottom-right-radius: 8px; border-bottom: none; }";
    html += ".fname { overflow: hidden; white-space: nowrap; text-overflow: ellipsis; max-width: 70%; }";
    html += ".del-btn { background-color: #cf6679; color: white; text-decoration: none; padding: 5px 10px; border-radius: 4px; font-size: 12px; font-weight: bold; }";

    // Footer
    html += ".footer { margin-top: 30px; font-size: 14px; color: #888; }";
    html += ".footer a { text-decoration: none; color: #03dac6; margin: 0 10px; font-weight: bold; }";
    html += "</style></head><body>";
    
    // --- CONTENT ---
    html += "<h2>Cheat Box Uploader</h2>";
    
    // Upload Card
    html += "<div class='card'>";
    html += "<h3 style='margin-top:0'>Upload New File</h3>";
    html += "<form method='POST' action='/upload' enctype='multipart/form-data'>";
    html += "<input type='file' name='f'><br>";
    html += "<input type='submit' value='Upload File'>";
    html += "</form></div>";

    // File List Card
    html += "<div class='card'>";
    html += "<h3 style='margin-top:0'>Current Files</h3>";
    html += "<ul>";
    
    // DYNAMICALLY LIST FILES FROM LITTLEFS
    File root = LittleFS.open("/");
    File file = root.openNextFile();
    bool filesFound = false;
    while(file){
        filesFound = true;
        String fileName = file.name();
        if(!fileName.startsWith("/")) fileName = "/" + fileName; // Ensure leading slash
        
        html += "<li>";
        html += "<span class='fname'>" + fileName + "</span>";
        // Link to Delete Handler
        html += "<a href='/delete?name=" + fileName + "' class='del-btn'>DELETE</a>";
        html += "</li>";
        
        file = root.openNextFile();
    }
    if(!filesFound) html += "<li style='justify-content:center; color:#777;'>No files found</li>";
    
    html += "</ul></div>";

    // Original Footer
    html += "<div class='footer'>";
    html += "<p>Follow my projects:</p>";
    html += "<a href='https://www.instagram.com/un_knownengineer/'>Instagram</a> | "; 
    html += "<a href='https://github.com/Screen-sLaYeR'>GitHub</a> | ";
    html += "<a href='https://www.youtube.com/channel/UCZY7nlVlLNcErflaa3IFR5g'>YouTube</a>";
    html += "</div>";
    
    html += "</body></html>";
    server.send(200, "text/html", html);
  });

  // 2. DELETE HANDLER (New!)
  server.on("/delete", HTTP_GET, [](){
    if(server.hasArg("name")) {
      String filename = server.arg("name");
      if(LittleFS.exists(filename)) {
        LittleFS.remove(filename);
      }
    }
    // Redirect back to main page to refresh the list
    server.sendHeader("Location", "/");
    server.send(303);
  });

  // 3. UPLOAD HANDLER
  server.on("/upload", HTTP_POST, [](){ 
    // On success, link back to home
    String html = "<html><body style='background:#121212; color:white; text-align:center; font-family:sans-serif; padding:50px;'>";
    html += "<h2>File Uploaded!</h2>";
    html += "<a href='/' style='color:#03dac6; font-size:20px;'>Back to File List</a>";
    html += "</body></html>";
    server.send(200, "text/html", html); 
  }, [](){
    HTTPUpload& upload = server.upload();
    if(upload.status == UPLOAD_FILE_START){
      String filename = upload.filename;
      if(!filename.startsWith("/")) filename = "/" + filename;
      File f = LittleFS.open(filename, "w");
      f.close();
    } else if(upload.status == UPLOAD_FILE_WRITE){
      File f = LittleFS.open("/" + upload.filename, "a");
      if(f) f.write(upload.buf, upload.currentSize);
      f.close();
    }
  });

  // ======================================================
  //              RTOS TASKS
  // ======================================================
  xTaskCreatePinnedToCore(taskUI_code, "UI", 8192, NULL, 1, &TaskUI, 1);
  xTaskCreatePinnedToCore(taskServer_code, "SRV", 10000, NULL, 2, &TaskServer, 0);
}

void loop() {
  vTaskDelete(NULL); 
}

/* =========================================
   HELPERS
   ========================================= */
void drawList(String items[], int count, int selIndex) {
  display.clearDisplay();
  int visibleLines = 3; 
  if (selIndex < listScrollOffset) listScrollOffset = selIndex;
  if (selIndex >= listScrollOffset + visibleLines) listScrollOffset = selIndex - visibleLines + 1;

  for (int i = 0; i < visibleLines; i++) {
    int idx = listScrollOffset + i;
    if (idx >= count) break;
    display.setCursor(0, i * 11);
    display.print(idx == selIndex ? ">" : " "); 
    display.setCursor(10, i * 11);
    String n = items[idx];
    if(n.length() > 18) n = n.substring(0, 18);
    display.print(n);
  }
  display.display();
}

void drawFileViewer() {
  display.clearDisplay();
  for (int i = 0; i < 4; i++) {
    int line = viewScrollLine + i;
    if (line >= totalFileLines) break;
    display.setCursor(0, i * 8);
    display.print(getLine(currentFileContent, line));
  }
  display.display();
}

void drawUploadScreen(String status) {
  display.clearDisplay();
  display.setCursor(0, 0);
  display.println("WIFI ON (Low Pwr)");
  display.setCursor(0, 12);
  display.println("AP: ESP_Reader_S3");
  display.println("IP: 192.168.4.1");
  display.display();
}

void loadFiles() {
  fileCount = 0;
  fileList[fileCount++] = "[..] BACK"; 
  File root = LittleFS.open("/");
  File f = root.openNextFile();
  while (f && fileCount < 20) {
    String n = f.name();
    if(!n.startsWith("/")) n = "/" + n;
    fileList[fileCount++] = n;
    f = root.openNextFile();
  }
}

int countLines(String str) {
  int c = 1;
  for (int i=0; i < str.length(); i++) if (str[i] == '\n') c++;
  return c + (str.length() / 21);
}

String getLine(String str, int lineIdx) {
  int cur = 0; int start = 0; int chars = 0;
  for (int i = 0; i < str.length(); i++) {
    if (str[i] == '\n' || chars >= 21) {
      if (cur == lineIdx) return str.substring(start, i);
      cur++; chars = 0; start = (str[i] == '\n') ? i + 1 : i; 
    } else chars++;
  }
  if (cur == lineIdx) return str.substring(start);
  return "";
}