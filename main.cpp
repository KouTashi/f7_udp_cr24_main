/*
キャチロボ2024
ROS2から角度司令を受信する
エンコーダーの値をもとに角度に対してPIDをかける
メイン基板V2はMD3,MD4が使えない
PIDはタイマー割り込みに変更予定
2024/09/10
*/

/*
MD1: シューティングコンベアθ軸
MD2: シューティングコンベア送り
MD3: 使用不可
MD4: 使用不可
MD5: シューティングコンベアr軸
MD6: 空き
MD7: ソーティングコンベア詰まり防止
MD8: ソーティングコンベア送り
*/

#include "EthernetInterface.h"
#include "QEI.h"
#include "mbed.h"
#include "rtos.h"
#include <cstdint>

//---------------------------QEI---------------------------//
QEI ENC1(PC_0, PG_1, NC, 2048, QEI::X2_ENCODING);
QEI ENC2(PF_2, PC_3, NC, 2048, QEI::X2_ENCODING);
QEI ENC3(PD_4, PF_5, NC, 2048, QEI::X2_ENCODING);
QEI ENC4(PA_6, PF_7, NC, 2048, QEI::X2_ENCODING);
QEI ENC5(PE_8, PF_9, NC, 2048, QEI::X2_ENCODING);
QEI ENC6(PF_10, PD_11, NC, 2048, QEI::X2_ENCODING);

/*
QEI (A_ch, B_ch, index, int pulsesPerRev, QEI::X2_ENCODING)
index -> Xピン, １回転ごとに１パルス出力される？ 使わない場合はNCでok
pulsePerRev -> Resolution (PPR)を指す
X4も可,X4のほうが細かく取れる
データシート: https://jp.cuidevices.com/product/resource/amt10-v.pdf
*/
//---------------------------QEI---------------------------//

using ThisThread::sleep_for;

void receive(UDPSocket *receiver);

//---------------------------ピンの割り当て---------------------------//
PwmOut MD1P(PA_0);
PwmOut MD2P(PA_3);
PwmOut MD3P(PB_4);
PwmOut MD4P(PB_5);
PwmOut MD5P(PC_7);
PwmOut MD6P(PC_6);
PwmOut MD7P(PC_8);
PwmOut MD8P(PC_9);

DigitalOut MD1D(PD_2);
DigitalOut MD2D(PG_2);
DigitalOut MD3D(PG_3);
DigitalOut MD4D(PE_4);
DigitalOut MD5D(PD_5);
DigitalOut MD6D(PD_6);
DigitalOut MD7D(PD_7);
DigitalOut MD8D(PC_10);

PwmOut SERVO1(PB_1);
PwmOut SERVO2(PB_6);
PwmOut SERVO3(PD_13);
PwmOut SERVO4(PD_12);

DigitalIn SW1(PF_15);
DigitalIn SW2(PG_14);
DigitalIn SW3(PG_9);
DigitalIn SW4(PE_7);
//---------------------------ピンの割り当て---------------------------//

//---------------------------For PID---------------------------//
int Pulse[7];
int last_Pulse[7];
int dt = 0;
double dt_d = 0; // casted dt
double deg[7];

double target[7];
double Kp;
double Ki;
double Kd;
double Error[7];
double last_Error[7];
double Integral[7];
double Differential[7];
double Output[7];
double deg_limit;
double pwm_limit; // PWM出力制限　絶対に消すな
//---------------------------For PID---------------------------//

double mdd[9];
double mdp[9];

bool pos_ok = false;
const char pos_ok_msg[64] = "1";

int main() {

  //---------------------------PWM Settings---------------------------//
  MD1P.period_us(50);
  MD2P.period_us(50);
  MD3P.period_us(50);
  MD4P.period_us(50);
  MD5P.period_us(50);
  MD6P.period_us(50);
  MD7P.period_us(50);
  MD8P.period_us(50);
  /*
  50(us) = 1000(ms) / 20000(Hz) * 10^3
  MDに合わせて調整
  CytronのMDはPWM周波数が20kHzなので上式になる
  */
  //---------------------------PWM Settings---------------------------//

  //---------------------------UDP Settings---------------------------//

  // 送信先情報(F7)
  const char *destinationIP = "192.168.8.197";
  const uint16_t destinationPort = 4000;

  // 自機情報
  const char *myIP = "192.168.8.215";
  const char *myNetMask = "255.255.255.0";
  const uint16_t receivePort = 5000;

  // イーサネット経由でインターネットに接続するクラス
  EthernetInterface net;
  // IPアドレスとPortの組み合わせを格納しておくクラス（構造体でいいのでは？）
  SocketAddress destination, source, myData;
  // UDP通信関係のクラス予定
  UDPSocket udp;
  // 受信用スレッド
  Thread receiveThread;

  /* マイコンのネットワーク設定 */
  // DHCPはオフにする（静的にIPなどを設定するため）
  net.set_dhcp(false);
  // IPなど設定
  net.set_network(myIP, myNetMask, "");

  printf("Starting...\n");

  // マイコンをネットワークに接続
  if (net.connect() != 0) {
    printf("Network connection Error >_<\n");
    return -1;
  } else {
    printf("Network connection success ^_^\nIP:%s\n", myIP);
  }

  // UDPソケットをオープン
  udp.open(&net);

  // portをバインドする
  udp.bind(receivePort);

  // 送信先の情報を入力
  destination.set_ip_address(destinationIP);
  destination.set_port(destinationPort);

  //---------------------------UDP Settings---------------------------//

  // 受信用のスレッドをスタート
  receiveThread.start(callback(receive, &udp));

  receiveThread.join();
  udp.close();
  net.disconnect();
  return 0;
}

void receive(UDPSocket *receiver) { // UDP受信スレッド

  using namespace std::chrono;

  Timer t; // PIDで使うためのタイマー
  t.start();
  SocketAddress source;
  char buffer[64];

  int data[9] = {0, 0, 0, 0, 0, 0, 0, 0, 0};

  bool init = 0;

  //---------------------------PID parameters---------------------------//
  //ゲインを一括で設定する。個別で設定する場合はPIDのfor内に記述する
  Kp = 0.1;          // Pゲイン
  Ki = 0.01;         // Iゲイン
  Kd = 0.0;          // Dゲイン
  deg_limit = 360.0; //上限
  pwm_limit =
      0.8; // MDに出力されるデューテー比の上限を設定する。安全装置の役割を持つ
  //動作に影響するようなら#defineに変更
  //---------------------------PID parameters---------------------------//

  while (1) {
    memset(buffer, 0, sizeof(buffer));
    if (const int result =
            receiver->recvfrom(&source, buffer, sizeof(buffer)) < 0) {
      printf("Receive Error : %d", result);
    } else {

      //---------------------------受信したパケット（文字列）をintに変換---------------------------//

      char *ptr;
      int ptr_counter = 1;
      // カンマを区切りに文字列を分割
      // 1回目
      ptr = strtok(buffer, ",");
      data[1] = atoi(ptr); // intに変換

      // 2回目以降
      while (ptr != NULL) {
        ptr_counter++;
        // strtok関数により変更されたNULLのポインタが先頭
        ptr = strtok(NULL, ",");
        data[ptr_counter] = atoi(ptr); // intに変換

        // ptrがNULLの場合エラーが発生するので対処
        if (ptr != NULL) {
          // printf("%s\n", ptr);
        }
      }
      //---------------------------受信したパケット（文字列）をintに変換---------------------------//

      //---------------------------方向指令を分離---------------------------//
      /*printf("%d, %d, %d, %d, %d\n", data[1], data[2], data[3], data[4],
             data[5]);*/

      for (int i = 1; i <= 9; i++) {
        if (data[i] > 0) {
          mdd[i] = 1;
        } else if (data[i] < 0) {
          mdd[i] = 0;
        }
        // 0.0~1.0の範囲にマッピング
        // mdp[i] = fabs(data[i]) / 255;
      }
      //---------------------------方向指令と速度指令を分離---------------------------//

      //---------------------------エンコーダーの値をもとに角度（度数法）を計算---------------------------//
      Pulse[1] = ENC1.getPulses();
      Pulse[2] = ENC2.getPulses();
      Pulse[3] = ENC3.getPulses();
      Pulse[4] = ENC4.getPulses();
      Pulse[5] = ENC5.getPulses();
      Pulse[6] = ENC6.getPulses();

      for (int i = 1; i <= 6; i++) {
        deg[i] = Pulse[i] / 4096.0 * 360.0; // 現在の角度（度数法）を求める
        // printf("%f\n", deg[1]);
        // deg[i] = fabs(deg[i]);
      }
      // printf("%lf\n", deg[1]);
      /*
      ENC1.reset();
      ENC2.reset();
      ENC3.reset();
      ENC4.reset();
      ENC5.reset();
      ENC6.reset();
      */
      //---------------------------エンコーダーの値をもとに角度（度数法）を計算---------------------------//

      t.stop(); //タイマーを停止する、回転数の計算とPIDに使う
      dt = duration_cast<milliseconds>(t.elapsed_time())
               .count(); // msで前回からの経過時間を取得

      //---------------------------PID---------------------------//

      dt_d =
          (double)dt /
          1000000000.0; // dtをdouble型にキャストする、そのままだと大きすぎるので微小時間にする（タイマー割り込みに変更予定）def:1000000000.0

      for (int i = 1; i <= 6; i++) {
        target[i] = (double)data[i];
        Error[i] = target[i] - (deg[i]); // P制御,目標値と現在値の差分をとる
        Integral[i] += ((Error[i] + last_Error[i]) * dt_d /
                        2); // I制御,差分の時間積分、台形で近似して計算
        Differential[i] =
            (Error[i] - last_Error[i]) / dt_d; // D制御,差分の時間微分

        last_Error[i] = Error[i];

        //個別にゲイン調整
        if (i == 1) {
          // PIDなら0.1,0.01,0,0
          Kp = 0.1;  // Pゲイン
          Ki = 0.01; // Iゲイン
          Kd = 0.0;  // Dゲイン
        }

        Output[i] +=
            ((Kp * Error[i]) + (Ki * Integral[i]) +
             (Kd * Differential[i])); // PID制御,ゲインをかけて足し合わせる

        /*
        //振動を防ぐためにある程度の誤差は許容する
        if (fabs(Error[i]) <= 3.0) {
          Output[i] = 0;
          MD2P = 0.2;
        }
        */

        //振動を防ぐためにある程度の誤差は許容する
        if (fabs(Error[1]) <= 3.0) {
          Output[1] = 0;
          MD2P = 0.5;
        }

        int r_tg = 2048;
        double r_duty = 0.5;

        // r軸の位置初期化（初回のみ）
        if (init == false) {
          if (data[3] > 0) {
            while (SW1 == 0) {
              MD5D = 1;
              MD5P = r_duty;
            }
            MD5P = 0;
          }
          if (data[3] < 0) {
            while (SW2 == 0) {
              MD5D = 0;
              MD5P = r_duty;
            }
            MD5P = 0;
          }
          init = true;
        }
        // end

        // r軸位置合わせ（PID無効）
        if (data[3] == 1) {
          while (SW1 == 0) {
            MD5D = 1;
            MD5P = r_duty;
          }
          ENC3.reset();
          MD5P = 0;
        }

        if (data[3] == -1) {
          while (SW2 == 0) {
            MD5D = 0;
            MD5P = r_duty;
          }
          ENC3.reset();
          MD5P = 0;
        }

        if (data[3] == 2) {
          if (abs(Pulse[3]) <= r_tg) {
            MD5D = 0;
            MD5P = r_duty;
          } else {
            MD5P = 0;
          }
        }

        if (data[3] == -2) {
          if (abs(Pulse[3]) <= r_tg) {
            MD5D = 1;
            MD5P = r_duty;
          } else {
            MD5P = 0;
          }
        }

        // end
/*
        // θ軸位置合わせ（PID無効）
        if ((Pulse[1] >= data[1] * 10)&&(abs(Pulse[1] - data[1] * 10) >= 25)) {
          MD1D = 0;
          MD1P = 0.3;
        } else if ((Pulse[1] <= data[1] * 10)&&(abs(Pulse[1] - data[1] * 10) >= 25)) {
          MD1D = 1;
          MD1P = 0.3;
        } else {
          MD1P = 0;
        }

        // end
*/
        mdp[i] = Output[i] / deg_limit;

        // 安全のためPWMの出力を制限　絶対に消すな
        if (mdp[i] > 0) {
          mdd[i] = 1;
        } else if (mdp[i] < 0) {
          mdd[i] = 0;
        }
        mdp[i] = fabs(mdp[i]);

        if (mdp[i] > pwm_limit) {
          mdp[i] = pwm_limit;
        }
        // end
      }

      //---------------------------PID---------------------------//

      t.reset(); //タイマーをリセット
      t.start(); //タイマーを開始
                 /*
                       printf("%lf, %lf, %lf, %lf\n", Output[1], Error[1], Integral[1],
                              Differential[1]);*/

      // モーターがうまく回らないときは要調整、短すぎるとPIDがうまく動かず、長すぎるとレスポンスが悪くなる
      sleep_for(10);

      //---------------------------モタドラに出力---------------------------//

      if (data[2] == 1) {
        MD2D = 1;
        MD2P = 0.3;
        MD8D = 0;
        MD8P = 0.3;
      }

      if (data[2] == 2) {
        MD2D = 0;
        MD2P = 0.3;
        MD8D = 0;
        MD8P = 0.3;
      }

      if (data[2] == 0) {
        MD2P = 0.0;
        MD8P = 0.0;
      }

      printf("%f\n",mdp[1]);

      MD1D = mdd[1];
      // MD2D = mdd[2];
      // MD3D = mdd[3];
      // MD4D = mdd[4];
      // MD5D = mdd[3];
      MD6D = mdd[1];
      MD7D = mdd[7];
      // MD8D = mdd[8];

      MD1P = mdp[1];
      // MD2P = mdp[2];
      // MD3P = mdp[3];
      // MD4P = mdp[4];
      // MD5P = mdp[3];
      MD6P = mdp[1];
      MD7P = 0.2;
      // MD8P = mdp[8];

      //---------------------------モタドラに出力---------------------------//
    }
  }
}
