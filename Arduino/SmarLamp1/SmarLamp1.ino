//----------------------------------------------------------------------
// программа управляемой розетки с включением/отключением по времени
// version 0.1.1  23/08/2019
// Cpyright Pridatko S.D., Pridatko D.Y.
//----------------------------------------------------------------------

#include <AltSoftSerial.h>  //новая библиотека последовательного порта. пин 9 - ТХ, пин 8 - RX, не изменяется. Установить длину буфера в библиотеке не менее 120 байт
#include <iarduino_RTC.h> 
#include <EEPROM.h>
#include <uTimerLib.h>
#include <CyberLib.h> // новая библиотека для замены стандартных обращений к портам

//////////// константы апаратной конфигурации (номеров пинов)
const byte RST = 5;
const byte CLK = 6;
const byte DAT = 7;
const int tCycle = 100; // время цикла основной программы
const int stEEPROM = 16; //стартовый адрес для записи EEPROM
const int piezoPin = 3;
const int ButPin = 11; //пин кнопки
const int LedPin = 13; //пин встроенного светодиода
// константы управления мощностью через симмистор
const int pPower=12; //  выход для управления симистором (9 или 10 использовать нельзя при этой библиотеке)
const int pSync=2; //вход для синхронизации с переходом через ноль сети (так как требуются прерывания по перходу через 0, то можно использовать только цифровые входы 2 и 3
const int Tic = 100; // 100 мкс - время одного тика таймера, обеспечивает управление мощностью нагрузки с точностью 1%

//////////// прочие константы
enum State {ON, OFF, ALARM };  //перечисляемый тип для состояния контроллера, ALARM - включено по будильнику, только при OFF - будильники проверяются на срабатывание
enum State state;  //переменная состояния контроллера

// структура для хранения параметров сети
// фиксированной длины иначе не сохранить в энергонезависимую память
struct NET {
// везде добавлен 1 байт на ноль в конце строки  
char ssid[33];  
char pass[33];
char IP[16];
char PortUDP[6];
};

struct NET net;

String OK = "\r\nOK\r\n";  // константа для нормального ответа от Wi-Fi !почему то сначала возврат каретки , а потом перевод строки, но по кодам из COM порта наоборот

String StringWiFi=""; //"q!!  !qqq$ON!dаdjj!iou!ppp$g!yy$ysd$SetAP+!j$ttt!ggg!xxx$$$hhh!!!aaa!$ff$!d$Set";  // строка с информацией полученной по Wi-Fi
// символ начала команды $, символ окончания команды !

//////////////////////////     основные глобальные переменные   ////////////////////////////////

int hotRestart = 0x0000;  //признак горячего рестарта 0x0F0F - для подключения к сети без звуков, если 0x0000 - то это холодный старт

// структура для хранения параметров конкретного будильника
struct WAKEUP {
  byte hour, minute; // время срабатывания
  bool week[7]; // признак срабатывания по дням недели ( Пн - 0)
  bool repeat; //признак многократного повтора будильника ( если false - то будильник разовый) - НЕ РЕАЛИЗОВАНО в данной версии!
  int timeon, MaxPower, tMaxPower; // время работы лампы в минутах, максимальная мощность лампы и время разгорания до максимальной мощности в минутах
  };
struct WAKEUP WakeUp[8];  // массив будильников

bool AlarmOn[8];  // массив признаков включения/ отключения будильников ( true - будильник действует)

String FormatOfInst[6] = { "ON", "OFF", "PWR+", "AOn+", "WUp+", "SetT+"};  //массив с названиями команд

AltSoftSerial WiFi;  //  создаем последовательный канал с Wi-Fi

iarduino_RTC time(RTC_DS1302, RST, CLK, DAT);  // создаем подключение с RTC

float FPower = 0; //переменная мощность с 10-ной дробью для плавного увеличения
int CurAlarm; // номер текущего сработавшего будильника

int OFFhour, OFFminute; //время выключения текущего будильника
float Step; // шаг приращения мощности при разгорании лампы

int num=0;
bool LedState = false;

////////////////////////  глобальные переменные для управления симисторным регулятором   ///////////////////////////
// глобальная переменная для обмена данными с процедурой прерывания
// переменная % мощности от 0 до 100
volatile int Power=0; 
volatile int delayTics=0;  //величина задержки на включение симистора ( от 98 до 0)


///////////////////////////        ФУНКЦИИ       ////////////////////////////////////////////
// функция программной перезагрузки
void(* resetFunc) (void) = 0;//объявляем функцию reset с адресом 0

//////////////////////////функции прерываний для управления мощностью - от 10% до 90% времени полупериода /////////////////////////

// в процедуре прерывания по переходу напряжения сети через 0 запускается таймер ( при этом симистор отключен)
// таймер срабатывает и вызывает процедуру triakOn, которая останавливает таймер и включает симистор

//---------------------------------------------------------------------------------------------
// процедура прерывания по таймеру 
void Takt(void) {
    if ( Power < 10 ) {  
      D12_High;  //замена digitalWrite(pPower, HIGH);  //отключаем симмистор на очень малых мощностях   
    } else if ( Power > 90 ) { 
            // на больших мощностях (>90 ) не отключаем , допуская случайное из -за задержек одних прерываний другими) сквозное включенное состояние между полупериодами
            D12_Low;  //фактически заменяет digitalWrite(pPower, LOW); // включить симмистор  
            } else { if ( delayTics == 0 ) {
                        // на малых и средних мощностях сложное управление - симмистор в нужный момент включаем и с задержкой тут же отключаем, 
                        // чтоб не было сквозного включения из-за несвоевременного снятия управляющего напряжения                   
                        D12_Low;  //фактически заменяет digitalWrite(pPower, LOW); // включить симмистор
                        // Generated by delay loop calculator  at http://www.bretmulvey.com/avrdelay.html
                        // Delay 48 cycles for 3us at 16 MHz
                        asm volatile (
                                      "    ldi  r18, 16" "\n"
                                      "1:  dec  r18"  "\n"
                                      "    brne 1b" "\n"
                                      );
                        D12_High;  //замена digitalWrite(pPower, HIGH);  //отключаем симмистор так как он уже открылся- здесь высокий уровень отключает/ низкий включает            
                      }; //if ( delayTics == 0 ) 
                    if ( delayTics >= 0 ) { delayTics--;}; // просто ведем отсчет оставшегося количества тактов до запуска симистора ( включая сам момент запуска)     
                   }       
}
//---------------------------------------------------------------------------------------------
// процедура прерывания по переходу напряжения через 0 
void zeroU(void) {
     //D12_High;  //замена digitalWrite(pPower, HIGH);  //отключаем симмистор в любом случае, чтоб не остался вкл. при переходе от высокой мощности к очень малой - здесь высокий уровень отключает/ низкий включает     
     delayTics = 100 - Power;
    // if (delayTics > 98) { delayTics = 98; };  //для избежания коллизий не более 98      можно убрать ????
}

//функция установки значения мощности с запретом прерывания
void SetPower(int pwr) {
  noInterrupts();
  Power = pwr;
  interrupts();  
}

// получить значение мощности
int GetPower() { 
  return Power;
  }

//  ---------------------   функции работы с EEPROM   ----------------------

// функция сброса данных в энергонезависимой памяти
void resetEEPROM(){
int EEadr = stEEPROM;
EEadr = EEadr + sizeof(net) + 8*sizeof(WakeUp[0])+ sizeof(AlarmOn);
for ( int i = stEEPROM; i <= EEadr; i++) { EEPROM.write(i, 0); }; 
}

//функция начальной загрузки данных
void loadEEPROM(){
// порядок считывания данных net, WakeUp[8], AlarmOn[8]
int EEadr = stEEPROM;
EEPROM.get(EEadr, net);
EEadr = EEadr + sizeof(net); 
for (int i = 0; i<8; i++){
  EEPROM.get(EEadr,WakeUp[i]);
  EEadr = EEadr + sizeof(WakeUp[0]); 
  }
  
EEPROM.get(EEadr,AlarmOn);
}

//сохранение только net
void saveNET(){
int EEadr = stEEPROM;
EEPROM.put(EEadr, net);
}

//сохранение только будильников
void saveWakeUp_i(int i){
int EEadr = stEEPROM;
EEadr = EEadr + sizeof(net) + i*sizeof(WakeUp[0]);

EEPROM.put(EEadr,WakeUp[i]);
}

//сохранение только статуса будильника
void saveALARMON(){
int EEadr = stEEPROM;
EEadr = EEadr + sizeof(net) + 8*sizeof(WakeUp[0]) ; 
EEPROM.put(EEadr,AlarmOn);
}

// --------------------------------  функции работы с WiFi -------------------

// чтение по байтно из буфера Wi-Fi
String readLineWiFi() {
  String S="";
  while (WiFi.available() > 0){ S.concat( (char) WiFi.read()); }   
  return S; 
}

// функция определяющая номер первой команды по массиву FormatOfInst полученой по Wi-Fi, если команды нет, то возвращает -1
int GetInstr( String *Instr ) {  
   int NInstr = -1;  
   *Instr="";
   int index;
   String cmd; //временная переменная для обработки текста команды
   StringWiFi = StringWiFi + readLineWiFi();  //читаем последние данные из буфера WiFi  и добовляем к строке
   while (NInstr == -1){ // пока не нашли команду   
   index = StringWiFi.indexOf("$");//Ищем первый $
   if (index != -1) {
   StringWiFi.remove(0, index);  // отбрасываем из строки начало до $ ($ оставляем)
   index = StringWiFi.indexOf("!",1);// Ищем символ конца команды - !
       if (index != -1) {        
       // сохраняем в переменную как команду, при этом удаляем лишние ведущие(в начале строки) символы начала команды $ (если они есть) с подстроками до последнего $ перед найденным выше !
       // это защита от повторяющихся символов начала команды, без символа окончания команды     
       cmd = StringWiFi.substring(StringWiFi.lastIndexOf("$",index-1)+1, index); 
       
       StringWiFi.remove(0, index+1);  //вырезаем команду с символами начала и окончания команды
       for (int i=0; i<6; i++) {//В цикле перебираем массив форматов команд и сравниваем, определяя есть ли команда, если команда есть то выходим из функции
          if ( cmd.startsWith(FormatOfInst[i]) ) { NInstr = i; break;}
        }
      }//(index != -1) - конец команды
      else 
        { index = StringWiFi.indexOf("$",1);//Ищем еще одно начало команды ( первый $)
          //  если найдено начало команды опять , то ошибка и надо стереть до нового начала команды
          if (index != -1) { 
            StringWiFi.remove(0,1); //index);             
          } 
          else { break; }  // если не нашли символа окончания команды, то выходим, т.к. команду еще всю не приняли      
        } // end (index != -1) - конец команды
   } //(index != -1) (1) 
   else {StringWiFi.remove(0); break; }  // если не нашли стартового $ ,то нет начала команды и стираем строку до конца и выходим из while
} // end while (NInstr == -1)

 if ( NInstr != -1 ) {*Instr = cmd;} // команду в цикле поиска нашли и присваиваем возвращаемой переменной
 else { *Instr = ""; }; // если команду не нашли, то сбрасываем строку команды 
//Serial.print("Command="); 
// Serial.println(*Instr); 
 return NInstr; 
 
} // end


// инициализация и настройка модуля Wi-Fi на работу клиентом и прием UDP сообщений ( команд от смартфона)
bool setupWiFi (String ssid, String pass, String IP, String PortUDP) {  
  String answer;
  bool error = false;
  String s="";
  String IPbroadcast = "";
  /*
        Serial.println("in setup WiFi");
        Serial.print("ssid="); Serial.println(ssid);
        Serial.print("Pass="); Serial.println(pass);
        Serial.print("IP="); Serial.println(IP);
        Serial.print("Port="); Serial.println(PortUDP);
*/

  ///      сброс       ///
  // при перезагрузке успешность прохождения команды не контролируем
  WiFi.println("AT+RST");
  delay (3000);
  answer = readLineWiFi();
  
  /// отключаем эхо команд    ///
  WiFi.println("ATE0");
  delay (100);
  answer = readLineWiFi();
  if ( answer.indexOf(OK) == -1 ) {error = true;}

  ///     модуль в режим клиента     ///
  WiFi.println("AT+CWMODE=1");   // здесь было 3 - это НЕ ПРАВИЛЬНО!!!
  delay (200);
  answer = readLineWiFi();
  if (!(answer.equals(OK) ) ) {error = true;}

  ///   подключаемся к точке доступа  ///
  s="AT+CWJAP=\""+ssid+"\",\""+pass+"\"";
  WiFi.println(s);
  delay (8000);
  answer = readLineWiFi();
  if ( answer.indexOf(OK) == -1 ) {error = true;}

  //  установка адреса вручную AT+CIPSTA="192.168.1.201"
  // WiFi.println("AT+CIFSR");  // определение адреса назначенного автоматически 
  s="AT+CIPSTA=\""+ IP + "\"";
  WiFi.println(s);
  delay (300);
  answer = readLineWiFi();

  /// разрешаем множественные соединения ///
  WiFi.println("AT+CIPMUX=1");
  delay (100);
  answer = readLineWiFi();
  if (!(answer.equals(OK) ) ) {error = true;}

  // на всякий случай разорвать UDP соединение 
  WiFi.println("AT+CIPCLOSE=4");
  delay (100);
  answer = readLineWiFi();
 // проверку на ОК здесь не делаем, т.к. обычно соединения нет и будет ответ ERROR, что является нормальным
  
  ///   устанавливаем UDP соединение      ///
  IPbroadcast = IP.substring(0,IP.lastIndexOf(".")+1);
  IPbroadcast = IPbroadcast +"255";
  s="AT+CIPSTART=4,\"UDP\",\"" + IPbroadcast + "\"," + PortUDP + "," + PortUDP + ",0";
  WiFi.println(s);
  delay (300);
  answer = readLineWiFi();
  if ( answer.indexOf(OK) == -1 ) {error = true;}
  
 /* 
  WiFi.println("AT+CIPSTATUS");
  Serial.println("AT+CIPSTATUS");
  delay (1000);
  answer = readLineWiFi();
  Serial.println(answer);  
  */
  
  return error;
}


//функция перевода в режим Access Point
bool setupWiFi_AP() {
  
  String answer;
  bool error = false;
  ///      сброс       ///
  // при перезагрузке успешность прохождения команды не контролируем
  WiFi.println("AT+RST");
  delay (3000);
  answer = readLineWiFi();
  
  /// отключаем эхо команд    ///
  WiFi.println("ATE0");
  delay (100);
  answer = readLineWiFi();
  if ( answer.indexOf(OK) == -1 ) {error = true;}
  
  ///     модуль в режим точки доступа     ///
  WiFi.println("AT+CWMODE=2");  
  delay (200);
  answer = readLineWiFi();
  if (!(answer.equals(OK) ) ) {error = true;}
  
  // повторно сброс
  WiFi.println("AT+RST");
  delay (3000);
  answer = readLineWiFi();
  if ( answer.indexOf(OK) == -1 ) {error = true;}
  
   /// отключаем эхо команд    ///
  WiFi.println("ATE0");
  delay (100);
  answer = readLineWiFi();
  if ( answer.indexOf(OK) == -1 ) {error = true;}

  String s="AT+CWSAP=\"SmartLamp\",\"s53l9amp7\",4,4";
  WiFi.println(s);
  delay (200);
  answer = readLineWiFi();
  
  /// IP адрес ///
  //  установка адреса вручную всегда одинакового, чтоб клиент на смартфоне его знал заранее
  WiFi.println("AT+CIPAP=\"192.168.48.2\"");
  delay (200);
  answer = readLineWiFi();
 
  /// разрешаем множественные соединения ///
  WiFi.println("AT+CIPMUX=1");  
  delay (100);
  answer = readLineWiFi();
  if (!(answer.equals(OK) ) ) {error = true;}
  
  // на всякий случай разорвать UDP соединение 
  WiFi.println("AT+CIPCLOSE=4");
  delay (100);
  answer = readLineWiFi();
 // проверку на ОК здесь не делаем, т.к. обычно соединения нет и будет ответ ERROR, что является нормальным

  ///   устанавливаем UDP соединение      ///
  //AT+CIPSTART=4,"UDP","192.168.48.3",2200,2200,0    - всегда одинаково, чтоб было подключение с клиентом на смартфоне
  s="AT+CIPSTART=4,\"UDP\",\"192.168.48.3\",2200,2200,0";
  WiFi.println(s);
  delay (1000);
  answer = readLineWiFi();
  if ( answer.indexOf(OK) == -1 ) {error = true;}

  /*WiFi.println("AT+CIPSTATUS");
  Serial.println("AT+CIPSTATUS");
  delay (1000);
  answer = readLineWiFi();
  Serial.println(answer);
  */
  return error;
}

//Функция настройки параметров сети с которой будет работать лампа: название сети, пароль, IP адрес лампы и порт
void SetUpNet(){
  String S = "";
  String tmp = "";
  int Len = 0;
    
  setupWiFi_AP(); //устанавливаем параметры сети как для точки доступа
  
  // т.к. идетификатор сети и пароль - поля переменной длины с любыми символами, то перед ними передаються длины полей
  //ждем и читаем начало команды
  //пока длина команды меньше минимальной формируем строку S из буфера
  while (  S.length() < 16  ) { S = S + readLineWiFi();  } 
  delay(1000); //ждем чтоб пришла основная часть 
  S = S + readLineWiFi(); // дочитываем разом
  // нв всякий случай читаем до конца
  while ( S.substring(S.length()-1) != "!" ) { S = S + readLineWiFi();  } 
  
  // тестовый ввод 
  // S = "$SetNet+11+MyWiFi+Net!+7+qwe!rty+192.168.1.150+2200!";
  
  // разбор команды
  S.remove(0, S.indexOf("$"));  // отбрасываем из строки начало до $ ($ оставляем)  
  tmp = S.substring(0,S.indexOf("+")+1);  // выделяем команду
  // проверяем , что пришла нужная команда
  if ( tmp.equals("$SetNet+") )  {
        S.remove(0,S.indexOf("+")+1);// обрезаем саму команду
        //ssid 
        // выделяем длину
        tmp =S.substring(0,S.indexOf("+"));
        Len = S.toInt();
        S.remove(0, S.indexOf("+")+1);  // отбрасываем из строки данные по длине  с символом +
        tmp =S.substring(0,Len); // выделяем ssid
        if (Len>32) {Len = 32;};  //ограничиваем длину строки
        for (int i = 0; i< Len; i++){net.ssid[i] = tmp.charAt(i);} //записываем в структуру значение SSID (переводим строку в чар с помощью спец. функции)
        net.ssid[Len] = '\0';
              
        // pass
        S.remove(0,Len+1); //обрезаем команду, которую уже запомнили
        // выделяем длину
        tmp =S.substring(0,S.indexOf("+"));
        Len = S.toInt();
        S.remove(0, S.indexOf("+")+1);  // отбрасываем из строки данные по длине  с символом +
        tmp =S.substring(0,Len); // выделяем pass
        if (Len>32) {Len = 32;};  //ограничиваем длину строки
        for (int i = 0; i< Len; i++){net.pass[i] = tmp.charAt(i);} //записываем в структуру значение SSID (переводим строку в чар с помощью спец. функции)
        net.pass[Len] = '\0';
               
        // IP
        S.remove(0,Len+1); //обрезаем команду, которую уже запомнили
        // тут поле не фиксированной длины, но + в нем быть не может, по этому ориентируемся на следующий +
        tmp = S.substring(0,S.indexOf("+"));
        Len = tmp.length();
        if (Len>15) {Len = 15;};  //ограничиваем длину строки
        for (int i = 0; i<Len; i++){net.IP[i] = tmp.charAt(i);} //записываем в структуру IP розетки
        net.IP[Len] = '\0';

        // port
        S.remove(0,S.indexOf("+")+1); //обрезаем команду, которую уже запомнили        
        tmp = S.substring(0,S.indexOf("!"));
        //записываем в структуру Port розетки
        Len = tmp.length();
        if (Len>5) {Len = 5;};  //ограничиваем длину строки
        for (int i = 0; i<Len; i++){ net.PortUDP[i] = tmp.charAt(i); } 
        net.PortUDP[Len] = '\0';
               
        /* Serial.print("ssid="); Serial.println(net.ssid);
        Serial.print("Pass="); Serial.println(net.pass);
        Serial.print("IP="); Serial.println(net.IP);
        Serial.print("Port="); Serial.println(net.PortUDP); */

        saveNET(); //запоминание в энергонезависимую память
        }
} // end SetUpNet()
  
//Функция звукового сигнала
void MyTone(byte pin, long f, long ms){
  pinMode(pin, OUTPUT);
  long Pause = 500000/f; // длительность полуволны = период/2 = 1000000/2/f= 500000/f
  long n = (ms*f)/1000; 
  for( long i = 0; i < n; i++){
      digitalWrite(pin, HIGH); delayMicroseconds(Pause); digitalWrite(pin, LOW); delayMicroseconds(Pause);
  }
}

//Функция звукового сигнала при превышении времени основного цикла 
void timeErrorTone(){
    //указательный сигнал Частота, Гц: 950, 1400, 1800; сигнал = 3 частоты подряд в заданой последовательности (каждая по 330 мс) = 1 с, пауза 1 с
    MyTone(piezoPin, 950, 330); MyTone(piezoPin, 1400, 330); MyTone(piezoPin, 1800, 330);  
  }

void ConnectTone(){
  // три длинных гудка
  MyTone(piezoPin, 425, 1000); delay(2000); MyTone(piezoPin, 425, 1000); delay(2000); MyTone(piezoPin, 425, 1000); delay(2000); 
  }

void ConnectErrorTone(){
   // занято
   for( int i = 0; i <10; i++) {   MyTone(piezoPin, 425, 100); delay(200);  };
  }

void ConnectSuccesTone(){
   MyTone(piezoPin, 3000, 350);
   for( int i = 0; i <40; i++) {   MyTone(piezoPin, 800+300*random(0,10),20);  };
  }  

void SetupTone(){
   // пи-пи-пи.... пи-пи-пи....
   for (int i = 0; i < 6; i++) {
   MyTone(piezoPin, 2000, 60); delay(30); MyTone(piezoPin, 2000, 60); delay(30); MyTone(piezoPin, 2000, 60); delay(30);
   delay(1000);
   }
  }  
   
// ========================================================================     ФУНКЦИЯ ИНИЦИАЛИЗАЦИИ    =======================================================================  
void setup() {
  
 EEPROM.get(0, hotRestart);
 int coldRestart = 0x0000; // используем переменную только чтоб писалось в память сразу два байта
 EEPROM.put(0, coldRestart);  //сбрасываем признак того, что перезагрузка горячая
 
//инициализация прерываний для управления мощностью
  SetPower(0);  
 // resetEEPROM(); -только для сброса данных
 D2_In; //pinMode(pSync, INPUT);
 D2_High; //digitalWrite(pSync, HIGH);
 D12_Out; // pinMode(pPower, OUTPUT);
 D12_High; //digitalWrite(pPower, HIGH);
 //настраиваем пин для кнопки
 pinMode(ButPin, INPUT);
 digitalWrite(ButPin, HIGH);
 // настраиваем выход светодиода
 pinMode(LedPin, OUTPUT);
 digitalWrite(LedPin, LOW);
  
 // инициализируем таймер и подключаем его процедуру прерывания
 TimerLib.setInterval_us(Takt, Tic);
   
  // подключаем процедуру прерывания 0 ( цифровой вход 2) для датчика перехода напряжения через 0
  // датчик выдает "1" если напряжение приблизилось к 0 Вольт - поэтому RISING
  attachInterrupt(0, zeroU, RISING);
  
WiFi.begin(38400); // инициализация последовательного интерфейса с WiFi
Serial.begin(38400);
time.begin(); //Инициализация RTC
loadEEPROM(); // загрузка параметров из EEPROM

/*Serial.print("DO AlarmOn = "); 
for ( int i=0; i<8; i++ ) { Serial.print(AlarmOn[i]); }
Serial.println();

 for (int i=0; i<8; i++){Serial.print(i); Serial.print(" "); Serial.print(WakeUp[i].hour); Serial.print(":"); Serial.print(WakeUp[i].minute);  Serial.print("days"); 
          for(int k = 0; k<7; k++){
            Serial.print(WakeUp[i].week[k]);
            }
          Serial.print(" "); Serial.print(WakeUp[i].repeat); Serial.print(" "); Serial.print(WakeUp[i].timeon); Serial.print(" "); Serial.print(WakeUp[i].tMaxPower);
          Serial.print(" "); Serial.print(WakeUp[i].MaxPower); Serial.println();
 }  */ 

//Если нажата кнопка для настройки, то
if( digitalRead(ButPin) == LOW ){
  SetupTone(); 
  SetPower(40);
  SetUpNet();
  SetPower(0);
}

// начальная настройка Wi-Fi модуля и подключение к точке доступа
// делаем три попытки
for ( int i = 0; i < 3; i++) {   
    if ( hotRestart != 0x0F0F ) {ConnectTone(); };  // если нет признака горячего рестарта, то есть звук
    if (setupWiFi(net.ssid, net.pass, net.IP, net.PortUDP)) {
      if ( hotRestart != 0x0F0F ) { ConnectErrorTone(); };  // если нет признака горячего рестарта, то есть звук
      }
    else { 
      if ( hotRestart != 0x0F0F ) { ConnectSuccesTone(); };  // если нет признака горячего рестарта, то есть звук
      break; // выходим из цикла досрочно если успешно подключились
    };
delay(10000); // пауза между попытками подключения    
} // for i

state = OFF;

}


// ==========================================================================   ОСНОВНОЙ ЦИКЛ   ====================================================================================
void loop() {
 unsigned long beginTime =  millis();
   
 int w, m, h , sec; //текущее время (день недели, час, минута, секунда)
 String Command="";   // строка команды выделенная из приемного буфера вай-фай
 int NCommand=-1;  // переменная номера команды в массиве  FormatOfInst, -1 означает что команды сейчас нет

 String S;
 String tmp; //дополнительная переменная для преобразования строки в число
 int i;
 int tmpNum;
  
 NCommand = GetInstr( &Command );  //читаем команду из буфера , если есть

 if ( NCommand !=-1) {
  
 Serial.print("NCom= ");   Serial.println(NCommand);   Serial.print("Command= ");   Serial.println(Command);  
  
 switch ( NCommand )  // фактически код  команды совпадает с индексом элемента в массиве FormatOfInst
{
 case 0 : // "$ON"
 //!!! может сделать плавный пуск для сбережения лампочек ???
          SetPower(100);
          state = ON;
          break;
 case 1 : // "$OFF"
          SetPower(0);
          state = OFF;
          break;
 case 2 : // "$PWR+"
          if (state == ON) { 
             //вырезаем команду и оставшуюся величину превращаем в число;
             S = Command;
             S.remove(0,4);         
             tmpNum = S.toInt();
             // контролируем границы мощности строго от 0 до 100
             if (tmpNum < 0) {tmpNum = 0;}
             if (tmpNum > 100) {tmpNum = 100;}
             SetPower(tmpNum); //устанавливаем мощность             
            }
          break;
 case 3 : // "$AOn+"
          //вырезаем команду и обрабатываем данные;
          S = Command;
          S.remove(0,4);
          for ( i=0; i<8; i++ ) { AlarmOn[i] = (S.substring(i,i+1) == "1"); } 
          /*Serial.print("DO AlarmOn = "); 
          for ( i=0; i<8; i++ ) { Serial.print(AlarmOn[i]); }
          Serial.println(); */
          saveALARMON();
          break;
 case 4 : // "$WUp+"
           S = Command;
           Serial.println(S);
           S.remove(0,4);// обрезаем саму команду
           tmp = S.substring(0,1);
           i = tmp.toInt();// определяем номер будильника
           i--;// подгоняем из 1-8 в 0-7
           if ( i<0 ) { i = 0; };
           if ( i>7 ) { i = 7; };
           tmp = S.substring(2,4);
           // выделяем часы и минуты
           WakeUp[i].hour = tmp.toInt();
           tmp = S.substring(5,7);
           if ( WakeUp[i].hour <0 ) { WakeUp[i].hour = 0; };
           if ( WakeUp[i].hour > 23 ) { WakeUp[i].hour = 23; };
           WakeUp[i].minute = tmp.toInt();
           if (  WakeUp[i].minute <0 ) {  WakeUp[i].minute = 0; };
           if (  WakeUp[i].minute > 59 ) {  WakeUp[i].minute = 59; };
           // выделяем дни недели по которым срабатывает будильник
           for (int j=0; j<7; j++) { WakeUp[i].week[j] = (S.substring(j+8,j+9) == "1"); }
           WakeUp[i].repeat = (S.substring(16,17) == "1");// выделяем признак повтора
           tmp = S.substring(18,20);
           WakeUp[i].timeon = tmp.toInt();// выделяем время работы
           if (  WakeUp[i].timeon < 0 ) {  WakeUp[i].timeon = 0; };
           if (  WakeUp[i].timeon > 99 ) {  WakeUp[i].timeon = 99; };
           tmp = S.substring(21,23);
           WakeUp[i].tMaxPower = tmp.toInt();// выделяем время увеличения мощности до максимума
           if ( WakeUp[i].tMaxPower < 0 ) {  WakeUp[i].tMaxPower = 0; };
           if ( WakeUp[i].tMaxPower > 99 ) {  WakeUp[i].tMaxPower = 99; };
           tmp = S.substring(24,27);
           WakeUp[i].MaxPower = tmp.toInt();// выделяем максимальное значение 
           if ( WakeUp[i].MaxPower < 0 ) {  WakeUp[i].MaxPower = 0; };
           if ( WakeUp[i].MaxPower > 99 ) {  WakeUp[i].MaxPower = 99; };
           
          saveWakeUp_i(i);
          /*Serial.print(i); Serial.print(" "); Serial.print(WakeUp[i].hour); Serial.print(":"); Serial.print(WakeUp[i].minute);  Serial.print(" days "); 
          for(int k = 0; k<7; k++){
            Serial.print(WakeUp[i].week[k]);
            }
          Serial.print(" "); Serial.print(WakeUp[i].repeat); Serial.print(" "); Serial.print(WakeUp[i].timeon); Serial.print(" "); Serial.print(WakeUp[i].tMaxPower);
          Serial.print(" "); Serial.println(WakeUp[i].MaxPower);*/
          break;
 case 5 : // "$SetT+"
          S = Command;
          // w, h, m, sec
          S.remove(0,5);// обрезаем саму команду
          tmp = S.substring(0,1);//0.0
          w = tmp.toInt(); //определяем текущий день недели, час, минуты и секунды
          w--;  // корректировка w - день недели от 1 до 7 (один знак: 1- Пн, 7-воскресенье переделываем в 0 - Пн) - теперь день недели по правилам программы Пн - 0
          if ( w<0 ) { w = 0; };
          if ( w>6 ) { w = 6; };
          tmp = S.substring(2,4);
          h = tmp.toInt();
          if ( h<0 ) { h = 0; };
          if ( h>23 ) { h = 23; };
          tmp = S.substring(5,7);
          m = tmp.toInt();
          if ( m<0 ) { m = 0; };
          if ( m>59 ) { m = 59; };
          tmp = S.substring(8,10);
          sec = tmp.toInt();
          if ( sec < 0 ) { sec = 0; };
          if ( sec > 59 ) { sec = 59; };

          // перед записью специально  НЕ корректируем день недели в правила RTC Вс -0.
          time.settime(sec, m, h, -1, -1, -1, w); //установка текущего времени

          /*Serial.print("DO SetTime = SS MM HH Week"); 
          Serial.print(sec); Serial.print("  "); Serial.print(m); Serial.print("  "); Serial.print(h); Serial.print("  "); Serial.print(w); 
          Serial.println(); */
          break; 
 }
} // if ( NCommand !=-1)
 
// считываем время из RTC, w - день недели 
// после чтения НЕ корректируем день недели в правила RTC Вс -0.

time.gettime();
w = time.weekday;  
m = time.minutes;
h = time.Hours;
sec = time.seconds;

// ++++++++++++++             раз в сутки в 02:33:11 перезагрузка             ++++++++++++++++++++
// если лампа отключена и нужное время
if ( (state == OFF) && (h == 2) && (m == 33 ) && (sec == 11) ) {
  
hotRestart = 0x0F0F; //присваиваем флагу положительное значение
EEPROM.put(0, hotRestart); //записываем значение флага в энергонезависимую память
delay(20);// эта задержка нужна для гарантированного окончания записи в энергонезависимую память  3,3 мс на одну операцию

//вызываем reset  и розетка перезагрузится до начала следующего будильника (старт в первые 3 сек каждой минуты)
resetFunc();
};

// действия раз в секунду
num++;
if (num >9) { 
// мигаем светодиодом
if (LedState==true) { digitalWrite(LedPin, HIGH);}
else { digitalWrite(LedPin, LOW); }; 
LedState = !LedState;  
/*Serial.print(h);  Serial.print(" : ");  Serial.print(m);  Serial.print(" : ");
Serial.print(time.seconds);  Serial.print(" d-");  Serial.print(w); 
Serial.print(" STATE= ");  Serial.print(state);
Serial.print(" Alarm="); Serial.println(CurAlarm);
*/
// Serial.print(" OFF H "); Serial.print(OFFhour); Serial.print(" OFF M "); Serial.println(OFFminute);

num=0;
/* WiFi.println("AT+CIPSTATUS");
  Serial.println("AT+CIPSTATUS");
  delay (300);
 String answer = readLineWiFi();
  Serial.println(answer);  
  */
/*tmpNum = GetPower() + 1;
SetPower(tmpNum);
if (GetPower() == 100) { SetPower(0);};
Serial.print(" Power1-");  Serial.println(GetPower() );  */

}

// если состояние OFF то
if (state == OFF) {
// проверяем срабатывание будильников
  for (int i=0; i<8; i++){    
  // если  сработал  будильник
  //проверяем секунды, чтоб после первых 3-х секунд будильник уже не включался и его можно было выключить со смартфона
  if ((m == WakeUp[i].minute) && (h == WakeUp[i].hour) && WakeUp[i].week[w] && AlarmOn[i] && ( (sec >= 0) && (sec <= 3) ) ) { 
    state = ALARM; // то состояние = ALARM
    // сбрасываем мощность в 0
    FPower = 0;
    SetPower(0);
    CurAlarm = i;
     if (WakeUp[CurAlarm].tMaxPower != 0){ Step = (float)WakeUp[CurAlarm].MaxPower / ( (float)WakeUp[CurAlarm].tMaxPower * 60 ) * ( (float)tCycle / 1000.0 ); }   //  вычисляем шаг мощности на каждом цикле ( .0 обязательно )
         else {Step = WakeUp[CurAlarm].MaxPower;}  //если 0, то разгорается мгновенно за 1 шаг и шаг равен макс мощности
// Вычисляем время выключения будильника
     OFFhour = WakeUp[CurAlarm].hour;
     OFFminute = WakeUp[CurAlarm].minute + WakeUp[CurAlarm].timeon;
     if (OFFminute >= 60) {OFFhour = OFFhour + OFFminute / 60; OFFminute = OFFminute % 60;}
     if (OFFhour >=24) {OFFhour = OFFhour-24;}
        
  }
  } //for
}  //if (state == OFF)

// если состояние ALARM
if (state == ALARM) {
  // если максимальная мощность не достигнута, то увеличивеам на шаг 
  if(GetPower() < WakeUp[CurAlarm].MaxPower) {FPower = FPower + Step; tmpNum = (int)FPower; SetPower(tmpNum);}  //устанавливаем мощность регулятора 
     else { SetPower(WakeUp[CurAlarm].MaxPower); };  
  
  // если время работы истекло, то выключить и состояние OFF
 if ((h >= OFFhour) && (m >= OFFminute)) {state = OFF; SetPower(0); FPower = 0;}
 
} 

unsigned long endTime = millis();
unsigned long p = endTime - beginTime;
 if( p > 0 ){
  if (p <= tCycle){p = tCycle-p; delay(p);}
  else { timeErrorTone(); }
}

}
