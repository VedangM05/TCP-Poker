#include <iostream>
#include <thread>
#include <string>
#include <vector>
#include <algorithm>
#ifndef _WIN32
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#endif
#include <sstream>
#include <atomic>
#include <cctype> 
#include <signal.h>
#include <errno.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "Ws2_32.lib")
#endif

#ifdef _WIN32
using socket_t = SOCKET;
#define READSOCK(s,b,l) recv((SOCKET)(s), (char*)(b), (int)(l), 0)
#define CLOSESOCK(s) closesocket((SOCKET)(s))
#define INVALID_SOCKET_VAL INVALID_SOCKET
#else
using socket_t = int;
#define READSOCK(s,b,l) read((s),(b),(l))
#define CLOSESOCK(s) close((s))
#define INVALID_SOCKET_VAL (-1)
#endif

#define PORT 5555

// --- ANSI Color Codes ---
#define RESET   "\033[0m"
#define BOLD    "\033[1m"
#define RED     "\033[31m"   
#define GREEN   "\033[32m"     
#define YELLOW  "\033[33m"     
#define BLUE    "\033[34m"    
#define MAGENTA "\033[35m"      
#define CYAN    "\033[36m"     
#define WHITE   "\033[37m"      

socket_t g_sock = 0;
std::atomic<bool> g_myTurn{false};
std::vector<std::string> g_holeCards;
std::vector<std::string> g_communityCards;

static bool sendAll(int sock, const char* data, size_t len) {
    size_t total = 0;
    while (total < len) {
        int n = (int)send(sock, data + total, (int)(len - total), 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (n == 0) return false;
        total += static_cast<size_t>(n);
    }
    return true;
}

std::string displayCards(const std::vector<std::string> &cards) {
    std::stringstream ss;
    for (int line = 0; line < 5; line++) {
        for (const auto &c_str : cards) {
            if (c_str.empty()) continue;

            std::string rank = c_str.substr(0, c_str.size() - 1);
            std::string suit_letter(1, c_str.back());
            std::string displaySuit = "?";
            std::string color = WHITE; 

            if (suit_letter == "H") { displaySuit = "♥"; color = RED; }
            else if (suit_letter == "D") { displaySuit = "♦"; color = RED; }
            else if (suit_letter == "C") { displaySuit = "♣"; color = CYAN; } 
            else if (suit_letter == "S") { displaySuit = "♠"; color = CYAN; }

            if (line == 0) ss << "┌─────┐ ";
            else if (line == 1) ss << "│" << color << rank << RESET << (rank.size() == 1 ? "    │ " : "   │ ");
            else if (line == 2) ss << "│  " << color << displaySuit << RESET << "  │ ";
            else if (line == 3) ss << "│" << (rank.size() == 1 ? "    " : "   ") << color << rank << RESET << "│ ";
            else ss << "└─────┘ ";
        }
        ss << "\n";
    }
    return ss.str();
}

void receiveMessages() {
    char buffer[1024];
    std::string networkBuffer = "";

    while(true) {
        int valread = READSOCK(g_sock, buffer, 1023);
        if(valread <= 0) {
            std::cout << RED << "Disconnected from server." << RESET << std::endl;
            g_myTurn = false;
            CLOSESOCK(g_sock);
            exit(0);
        }
        buffer[valread] = '\0';
        networkBuffer += buffer;

        size_t pos;
        while ((pos = networkBuffer.find('\n')) != std::string::npos) {
            std::string msg = networkBuffer.substr(0, pos);
            networkBuffer.erase(0, pos + 1);
            msg.erase(std::remove(msg.begin(), msg.end(), '\r'), msg.end());

            if (msg.find("GAME_STARTING") != std::string::npos) {
                g_communityCards.clear();
                std::cout << "\n" << MAGENTA << "-------------------------------" << RESET << "\n";
                std::cout << BOLD << MAGENTA << "--- NEW HAND STARTING ---" << RESET << "\n" << std::endl;
            }
            else if (msg.find("YOUR_MOVE") != std::string::npos) {
                std::cout << "\n" << BOLD << CYAN << ">>> YOUR TURN TO ACT <<<" << RESET << std::endl;
                g_myTurn = true;
            }
            else if (msg.find("HOLE ") == 0) {
                g_holeCards.clear();
                std::string rest = msg.substr(5);
                std::stringstream ss_cards(rest);
                std::string card;
                while (ss_cards >> card) { g_holeCards.push_back(card); }
                std::cout << YELLOW << "--- Your Hole Cards ---" << RESET << std::endl;
                std::cout << displayCards(g_holeCards); 
            }
            else if (msg.find("CARDS ") == 0) {
                g_communityCards.clear();
                std::string rest = msg.substr(6);
                std::stringstream ss_cards(rest);
                std::string card;
                while (ss_cards >> card) { g_communityCards.push_back(card); }

                std::string stageName = "";
                if (g_communityCards.size() == 3) stageName = "FLOP";
                else if (g_communityCards.size() == 4) stageName = "TURN";
                else if (g_communityCards.size() == 5) stageName = "RIVER";

                if (!stageName.empty()) {
                     std::cout << "\n" << MAGENTA << "--- " << stageName << " ---" << RESET;
                }

                std::cout << "\n" << YELLOW << "--- Your Hand ---" << RESET << std::endl;
                std::cout << displayCards(g_holeCards);
                std::cout << YELLOW << "--- Community Cards ---" << RESET << std::endl;
                std::cout << displayCards(g_communityCards) << std::endl;
            }
            else if (msg.find("CHAT:") == 0) {
                std::string chat = msg.substr(5);
                size_t colonPos = chat.find(':');
                if (colonPos != std::string::npos) {
                    std::string name = chat.substr(0, colonPos);
                    std::string chatMsg = chat.substr(colonPos + 1);
                    std::cout << "[" << YELLOW << name << RESET << "]: " << chatMsg << std::endl;
                }
            }

                                 //SHOWDOWN//
            else if (msg.find("'s hand: ") != std::string::npos) {
                // Parse the message e.g., "AI_Bot's hand: 7D 8S"
                size_t nameEndPos = msg.find("'s hand: ");
                std::string namePart = msg.substr(0, nameEndPos + 10); // "AI_Bot's hand: "
                std::string cardData = msg.substr(nameEndPos + 10);    // "7D 8S"

                // Print the name part in yellow
                std::cout << YELLOW << namePart << RESET;

                // Now parse and print the cards with color
                std::stringstream ss_cards(cardData);
                std::string card;
                while (ss_cards >> card) {
                    if (card.empty()) continue;

                    std::string rank = card.substr(0, card.size() - 1);
                    std::string suit_letter(1, card.back());
                    std::string displaySuit = "?";
                    std::string color = WHITE; // Default color

                    // --- THIS IS THE COLOR LOGIC ---
                    if (suit_letter == "H") { displaySuit = "♥"; color = RED; }
                    else if (suit_letter == "D") { displaySuit = "♦"; color = RED; }
                    else if (suit_letter == "C") { displaySuit = "♣"; color = CYAN; }
                    else if (suit_letter == "S") { displaySuit = "♠"; color = CYAN; }
                    // --- END COLOR LOGIC ---

                    std::cout << color << rank << displaySuit << RESET << " ";
                }
                std::cout << std::endl; // End the line
            }

             else if (msg.find("Pot: ") != std::string::npos) {
                 // Color Pot line green
                 std::cout << GREEN << msg << RESET << std::endl;
             }
             else if (msg.find(" folds.") != std::string::npos) {
                 // Color fold action red
                 std::cout << RED << msg << RESET << std::endl;
             }
             else if (msg.find(" checks.") != std::string::npos || msg.find(" calls ") != std::string::npos) {
                 // Color check/call action yellow
                 std::cout << YELLOW << msg << RESET << std::endl;
             }
              else if (msg.find(" raises ") != std::string::npos) {
                 // Color raise action green
                 std::cout << GREEN << msg << RESET << std::endl;
             }
             else if (msg.find(" wins ") != std::string::npos || msg.find("Split pot!") != std::string::npos) {
                  // Color winner announcement green and bold
                 std::cout << BOLD << GREEN << msg << RESET << std::endl;
             }
             else if (msg.find("--- SHOWDOWN ---") != std::string::npos || msg.find("--- Hand Over ---") != std::string::npos) {
                  // Color delimiters magenta
                 std::cout << MAGENTA << msg << RESET << std::endl;
             }
            else {
                std::cout << msg << std::endl;
            }
        }
    }
}

            // ===== END SHOWDOWN =====//

// ===== Main Function =====
int main() {
#ifndef _WIN32
    signal(SIGPIPE, SIG_IGN);
#else
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2,2), &wsaData) != 0) {
        std::cout << "WSAStartup failed.\n"; return -1;
    }
#endif
    std::string serverIP, playerName;
    std::cout << "Enter server IP (e.g., 127.0.0.1): ";
    std::getline(std::cin, serverIP);
    std::cout << "Enter your player name: ";
    std::getline(std::cin, playerName);

    struct sockaddr_in serv_addr;
    g_sock = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
#ifdef _WIN32
    setsockopt(g_sock, SOL_SOCKET, SO_KEEPALIVE, (const char*)&one, sizeof(one));
#else
    setsockopt(g_sock, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof(one));
#endif
#ifdef SO_NOSIGPIPE
    setsockopt(g_sock, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(PORT);

    if (inet_pton(AF_INET, serverIP.c_str(), &serv_addr.sin_addr) <= 0) {
        std::cout << "Invalid address.\n"; return -1;
    }
    if (connect(g_sock, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
        std::cout << "Connection failed.\n"; return -1;
    }

    {
        std::string reg = playerName + "\n";
        if (!sendAll(g_sock, reg.c_str(), reg.size())) {
            std::cout << "Failed to send name to server.\n"; return -1;
        }
    }

    std::thread recvThread(receiveMessages);
    recvThread.detach();

    std::cout << "Connected! Waiting for game to start..." << std::endl;
    std::cout << "Type '/chat <msg>' to chat." << std::endl;
    std::cout << "Type 'FOLD', 'CALL', 'CHECK', or 'RAISE <amount>' when it's your turn." << std::endl;

    std::string input;
    while(std::getline(std::cin, input)) {
        if(input.empty()) continue;

        if (input.find("/chat ") == 0) {
            std::string chatMsg = "CHAT:" + input.substr(6) + "\n";
            if (!sendAll(g_sock, chatMsg.c_str(), chatMsg.size())) {
                std::cout << RED << "Disconnected from server." << RESET << std::endl; break;
            }
        }
        else if (g_myTurn) {
            std::string upperInput = input;
            std::transform(upperInput.begin(), upperInput.end(), upperInput.begin(), ::toupper);

            if (upperInput.find("FOLD") != 0 && upperInput.find("CALL") != 0 &&
                upperInput.find("RAISE") != 0 && upperInput.find("CHECK") != 0) {
                std::cout << "Invalid command. Use FOLD, CALL, CHECK, or RAISE <amount>." << std::endl;
            } else {
                std::string line = input + "\n";
                if (!sendAll(g_sock, line.c_str(), line.size())) {
                    std::cout << RED << "Disconnected from server." << RESET << std::endl; break;
                }
                g_myTurn = false;
            }
        }
        else {
            std::cout << "It's not your turn to make a move." << std::endl;
        }
    }

    CLOSESOCK(g_sock);
#ifdef _WIN32
    WSACleanup();
#endif
    return 0;
}
