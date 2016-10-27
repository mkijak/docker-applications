/**
 * receive TTN messages and export them to RabbitMQ
 *
 * 2016-10-11 Ab Reitsma
 */

import * as winston from "winston";
import * as iot from "./iotMsg";
import decodeProtoBuf from "./decodeProtobuf";


declare interface MessageKPN {
  LrrSNR: string;
  Lrrid: any;
  SpFact: number;
  SubBand: any;
  CustomerData: any;
  FPort: number;
  Channel: any;
  FCntUp: number;
  Time: any;
  DevEUI: string;
  payload_hex: string;
  CustomerID: string;
  LrrRSSI: string;
  ADRbit: number;
  ModelCfg: number;
  mic_hex: string;
  LrrLON: string;
  LrrLAT: string;
  FCntDn: number;
  Lrcid: any;
  DevLrrCnt: number;
}

export default class ReceiveKPN {
  receiver: iot.ReceiveMessages;
  sender: iot.SendMessages;

  constructor(receiver: iot.ReceiveMessages, sender: iot.SendMessages) {
    this.receiver = receiver;
    this.sender = sender;

    receiver.startConsumer((msg) => {
      this.messageConsumerKPN(msg);
    });
  }

  private messageConsumerKPN(msg) {
    try {
      var rawPayload = new Buffer(msg.payload_hex, "hex");
      var payload = decodeProtoBuf(rawPayload);

      var metadata: [iot.Metadata] = [{
        rssi: Number(msg.LrrRSSI),
        server_time: new Date(msg.Time).toISOString(),
        longitude: Number(msg.LrrLON),
        latitude: Number(msg.LrrLAT)
        // other metadata fields ignored for now
      }];

      // convert payload
      var messageIot = {
        payload: payload,
        port: msg.FPort,
        counter: msg.FCntUp,
        dev_eui: msg.DevEUI,
        metadata: metadata
      };

      // publish result
      winston.info("Message received from KPN.");
      this.sender.send(messageIot);
    } catch (err) {
      winston.error("Error receiving KPN message: " + err.message, err);
    }
  }
}
