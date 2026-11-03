#!/usr/bin/env node
// Triagem de mensagens de clientes do ChurchDAW com o Jev (TypeSafe).
//
//   node Tools/jev-suporte.mjs "texto da mensagem do cliente"
//   node Tools/jev-suporte.mjs --arquivo mensagens.txt     (uma mensagem por linha)
//
// Devolve tipo, area do app, urgencia e se o cliente esta frustrado, com as
// probabilidades. Cada leitura vai para ~/.config/churchdaw/suporte.jsonl
// (historico local, fora do repositorio). Chave: TYPESAFE_API_KEY ou
// ~/.config/churchdaw/typesafe.key.

import { appendFileSync, existsSync, mkdirSync, readFileSync } from "node:fs";
import { homedir } from "node:os";
import { join } from "node:path";

const configDir = join(homedir(), ".config", "churchdaw");
const keyFile = join(configDir, "typesafe.key");
const apiKey = (process.env.TYPESAFE_API_KEY ?? (existsSync(keyFile) ? readFileSync(keyFile, "utf8") : "")).trim();
if (!apiKey) { console.error("Sem chave da TypeSafe"); process.exit(2); }

const args = process.argv.slice(2);
const fileIndex = args.indexOf("--arquivo");
const messages = fileIndex >= 0
  ? readFileSync(args[fileIndex + 1], "utf8").split(/\r?\n/).map((l) => l.trim()).filter(Boolean)
  : [args.join(" ").trim()].filter(Boolean);
if (messages.length === 0) { console.error('Uso: node Tools/jev-suporte.mjs "mensagem"'); process.exit(2); }

const TIPOS = {
  bug: "Bug / defeito: algo não funciona como deveria, trava, some som, dá erro.",
  melhoria: "Pedido de melhoria ou crítica construtiva: funciona, mas poderia ser melhor.",
  duvida: "Dúvida de uso: a pessoa quer saber como fazer algo no app.",
  elogio: "Elogio / agradecimento, sem pedido.",
  reclamacao: "Reclamação / insatisfação com o produto ou o atendimento.",
  comercial: "Comercial: compra, licença, pagamento, preço, renovação.",
};
const AREAS = {
  som_motor: "Som e motor: timbres, samples, camadas de velocity, pipocos, estalos, latência, volume.",
  interface: "Interface e design: visual, telas, botões, layout.",
  midi_teclado: "MIDI e teclado controlador: teclas, pedal, mapeamento MIDI, Launchkey.",
  audio_interface: "Placa de som e driver: ASIO, interface de áudio, sem som, configuração de áudio.",
  plugins: "Plugins VST/AU: Pianoteq, Keyscape, BigSky, carregar plugin.",
  live_cam: "Live Cam / ChurchCam: gravação de vídeo, webcam.",
  instalacao: "Instalação, atualização, login e licença do app.",
  outro: "Outro assunto.",
};

async function classify(message) {
  const body = {
    model: "jev-latest",
    state: { produto: "ChurchDAW, DAW de palco para tecladistas de igreja", mensagem_do_cliente: message },
    questions: {
      tipo: { type: "choice", instructions: "Que tipo de mensagem é `mensagem_do_cliente`?", criteria: TIPOS },
      area: { type: "choice", instructions: "Sobre qual área do ChurchDAW é `mensagem_do_cliente`?", criteria: AREAS },
      urgencia: {
        type: "score",
        instructions: "Quão urgente é resolver o que `mensagem_do_cliente` relata?",
        criteria: [
          "Nenhuma: elogio ou ideia para o futuro.",
          "Baixa: melhoria que pode entrar numa próxima versão.",
          "Média: incomoda o uso, mas dá para tocar.",
          "Alta: atrapalha tocar no culto.",
          "Crítica: não consegue tocar / app não abre / perdeu o som no culto.",
        ],
      },
      frustrado: { type: "noul", instructions: "O cliente em `mensagem_do_cliente` está frustrado ou irritado?" },
    },
  };
  const response = await fetch("https://api.typesafe.ai/v1/systemone", {
    method: "POST",
    headers: { Authorization: `Bearer ${apiKey}`, "Content-Type": "application/json" },
    body: JSON.stringify(body),
  });
  if (!response.ok) throw new Error(`HTTP ${response.status}: ${(await response.text()).slice(0, 200)}`);
  return response.json();
}

mkdirSync(configDir, { recursive: true });
const urgencyNames = ["nenhuma", "baixa", "média", "alta", "crítica"];
for (const message of messages) {
  const { answers, model } = await classify(message);
  const urgency = urgencyNames[Math.round(answers.urgencia.score)];
  const pct = (p) => `${Math.round(p * 100)}%`;
  console.log(`\n"${message.slice(0, 160)}${message.length > 160 ? "…" : ""}"`);
  console.log(`  tipo:      ${answers.tipo.choice} (${pct(answers.tipo.probabilities[answers.tipo.choice])})`);
  console.log(`  área:      ${answers.area.choice} (${pct(answers.area.probabilities[answers.area.choice])})`);
  console.log(`  urgência:  ${urgency} (${answers.urgencia.score.toFixed(1)} de 4)`);
  console.log(`  frustrado: ${pct(answers.frustrado.noul)}`);
  appendFileSync(join(configDir, "suporte.jsonl"), JSON.stringify({
    em: new Date().toISOString(), modelo: model, mensagem: message,
    tipo: answers.tipo.choice, area: answers.area.choice,
    urgencia: answers.urgencia.score, frustrado: answers.frustrado.noul,
  }) + "\n");
}
