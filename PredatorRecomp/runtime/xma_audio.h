// Decodificador dos quadros de audio do XMA (Xbox 360). Cada quadro XMA carrega 512 amostras por canal no formato
// WMA Pro; o chip (xma.cpp) separa os quadros dos pacotes e entrega um por vez aqui. 1 ou 2 canais por fluxo.
#pragma once
#include <cstdint>
#include <memory>

class XmaAudioDecoder
{
public:
    XmaAudioDecoder();
    ~XmaAudioDecoder();
    // channels 1 ou 2; sampleRate 24000/32000/44100/48000. decodeFlags = opcoes de codificacao do XMA (0x10D6).
    bool Init(int channels, int sampleRate, uint32_t decodeFlags = 0x10D6);
    // Decodifica um quadro (bits comecando pelo campo de tamanho de 15 bits). Escreve 512 amostras por canal
    // (intercaladas se estereo) em out e retorna quantas amostras por canal escreveu (0 no primeiro quadro, que so
    // prepara a sobreposicao; 512 de silencio se o quadro estava corrompido).
    int DecodeFrame(const uint8_t* data, uint32_t bits, int16_t* out);
    int Channels() const;
    void Reset();   // esquece o estado entre quadros
    void Errors(uint64_t& frames, uint64_t& lengths) const;   // quadros corrompidos / com tamanho que nao bate
    int LastFail(int& bit) const;   // depuracao: em que etapa o ultimo quadro falhou (0 = nao falhou) e em que bit
    int LastLenDiff() const;        // depuracao: bits que sobraram no ultimo quadro (0 = bateu)
private:
    struct State;
    std::unique_ptr<State> s;
};
