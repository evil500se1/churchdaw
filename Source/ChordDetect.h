#pragma once

#include <juce_core/juce_core.h>

//==============================================================================
// Reconhecimento do acorde a partir das teclas pressionadas.
//
// A ideia: reduzir as notas a CLASSES DE ALTURA (dó em qualquer oitava e a mesma
// classe), testar cada uma das 12 como possivel fundamental e ver qual delas faz
// o conjunto casar com um formato conhecido.
//
// Regra de desempate que importa na pratica: quando dois formatos casam (Am7 e
// C6 tem exatamente as mesmas notas), ganha o que tem a fundamental no BAIXO --
// que e como o ouvido decide tambem.
//==============================================================================
struct ChordDetect
{
    struct Shape
    {
        juce::uint16 mask;      // intervalos a partir da fundamental, em bits
        const char* suffix;
        int priority;           // desempate quando o baixo nao ajuda
    };

    static juce::String nameFor (const bool held[128])
    {
        juce::uint16 classes = 0;
        int count = 0, lowest = -1;

        for (int n = 0; n < 128; ++n)
            if (held[n])
            {
                classes |= (juce::uint16) (1u << (n % 12));

                if (lowest < 0)
                    lowest = n;

                ++count;
            }

        if (count < 2 || lowest < 0)
            return {};

        // duas notas: intervalo, nao acorde. Quinta e oitava valem a pena mostrar.
        if (countBits (classes) == 2)
        {
            const int bassClass = lowest % 12;
            const int other = otherClass (classes, bassClass);

            if (other < 0)
                return {};

            const int iv = (other - bassClass + 12) % 12;

            if (iv == 7) return juce::String (noteName (bassClass)) + "5";

            return {};
        }

        static const Shape shapes[] =
        {
            { 0b000010010001, "",     10 },   // maior      0 4 7
            { 0b000010001001, "m",    10 },   // menor      0 3 7
            { 0b000001001001, "dim",   4 },   //            0 3 6
            { 0b000100010001, "aug",   3 },   //            0 4 8
            { 0b000010000101, "sus2",  5 },   //            0 2 7
            { 0b000010100001, "sus4",  6 },   //            0 5 7
            { 0b100010010001, "maj7",  9 },   //            0 4 7 11
            { 0b010010010001, "7",     9 },   //            0 4 7 10
            { 0b010010001001, "m7",    9 },   //            0 3 7 10
            { 0b010001001001, "m7b5",  4 },   //            0 3 6 10
            { 0b001001001001, "dim7",  3 },   //            0 3 6 9
            { 0b001010010001, "6",     7 },   //            0 4 7 9
            { 0b001010001001, "m6",    6 },   //            0 3 7 9
            { 0b000010010101, "add9",  7 },   //            0 2 4 7
            { 0b010010010101, "9",     8 },   //            0 2 4 7 10
            { 0b100010010101, "maj9",  8 },   //            0 2 4 7 11
            { 0b010010001101, "m9",    8 },   //            0 2 3 7 10
        };

        const int bassClass = lowest % 12;

        int bestRoot = -1, bestScore = -1;
        const char* bestSuffix = "";

        for (int root = 0; root < 12; ++root)
        {
            const juce::uint16 rel = rotateDown (classes, root);

            for (const auto& s : shapes)
            {
                if (rel != s.mask)
                    continue;

                // fundamental no baixo pesa mais que qualquer preferencia de forma
                const int score = s.priority + (root == bassClass ? 100 : 0);

                if (score > bestScore)
                {
                    bestScore = score;
                    bestRoot = root;
                    bestSuffix = s.suffix;
                }
            }
        }

        if (bestRoot < 0)
            return {};

        juce::String out (noteName (bestRoot));
        out << bestSuffix;

        // inversao: mostra a nota do baixo, como em cifra ("C/G")
        if (bestRoot != bassClass)
            out << "/" << noteName (bassClass);

        return out;
    }

private:
    static const char* noteName (int pitchClass)
    {
        static const char* const n[12] = { "C", "C#", "D", "D#", "E", "F",
                                           "F#", "G", "G#", "A", "A#", "B" };
        return n[((pitchClass % 12) + 12) % 12];
    }

    static int countBits (juce::uint16 v)
    {
        int c = 0;

        while (v) { c += (v & 1); v >>= 1; }

        return c;
    }

    static int otherClass (juce::uint16 classes, int notThis)
    {
        for (int i = 0; i < 12; ++i)
            if (i != notThis && (classes & (1u << i)) != 0)
                return i;

        return -1;
    }

    // gira o conjunto de classes para que "root" vire o bit 0
    static juce::uint16 rotateDown (juce::uint16 classes, int root)
    {
        juce::uint16 out = 0;

        for (int i = 0; i < 12; ++i)
            if ((classes & (1u << i)) != 0)
                out |= (juce::uint16) (1u << (((i - root) % 12 + 12) % 12));

        return out;
    }
};
