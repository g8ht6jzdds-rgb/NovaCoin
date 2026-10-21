#include <fcntl.h>
#include <string_view>
#include <sys/socket.h>
#include <unistd.h>

int main(const int argc, char* argv[])
{
    if (argc != 2) {
        return 2;
    }
    if (std::string_view{argv[1]} == "socket") {
        const int descriptor = socket(AF_INET, SOCK_STREAM, 0);
        if (descriptor >= 0) {
            static_cast<void>(close(descriptor));
        }
    } else {
        const int descriptor = open("must-not-exist", O_CREAT | O_WRONLY, 0600);
        if (descriptor >= 0) {
            static_cast<void>(close(descriptor));
        }
    }
    return 0;
}
