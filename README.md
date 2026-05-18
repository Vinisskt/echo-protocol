# Echo Protocol 🔊

A high-performance network protocol for Linux designed to tunnel traffic through audio frequencies using AFSK (Audio Frequency Shift Keying). Built with a focus on bit-level efficiency, low latency, and rigorous verification.

## Architecture

The protocol leverages several key technologies to achieve reliable data transmission over sound:

* AFSK Modulation: Audio Frequency Shift Keying for data encoding.
* Goertzel Algorithm: Efficient signal detection and frequency identification.
* Bit-level Ring Buffer: Custom high-performance buffer management for raw bitstreams.
* TUN/TAP Interface: Seamless integration with the Linux network stack.

## Status do Projeto

*   **Ring Buffer:** 100% Funcional (Manipulação de bits).
*   **Modulação AFSK:** 100% Funcional (Fase contínua).
*   **Demodulação AFSK:** 100% Funcional (Algoritmo de Goertzel).

## Resiliência

O sistema demonstrou alta confiabilidade em ambientes ruidosos, mantendo **0% de erro de bit (BER)** em testes de estresse com até **125% de ruído branco** em relação à amplitude do sinal original.

## Development Standards

* Performance: Optimized C implementation targeting low-latency and minimal resource footprint.
* Testing: Mandatory unit test coverage for every internal function.
* Documentation: Detailed technical documentation for every library component.

## Project Structure

* include/: API definitions and headers.
* src/: Core implementation.
* tests/: Comprehensive test suite.
* docs/: Detailed technical specifications and library documentation.

## How to Test

The project includes a comprehensive test suite to verify bit-level logic and buffer integrity. To compile and execute all tests, use the provided Makefile in the tests directory:

1. Navigate to the tests directory:
   cd tests

2. Run the tests:
   make run_tests

3. To clean build artifacts:
   make clean

## License

This project is licensed under the GNU Affero General Public License v3.0 (AGPL-3.0). See the LICENSE file for more details.
