import openai
import os

OpenAI_APIKEY = "YOUR-API-KEY"
client = openai.OpenAI(api_key=OpenAI_APIKEY)


base_prompt = """
Create a complete Lua simulation script for a realistic industrial process, based on the provided template. The simulation must:

- Be self-contained (no external dependencies)
- Use only integer-safe logic for all calculations and outputs
- Include Modbus-style variables (Coils, DiscreteInputs, HoldingRegisters, InputRegisters)
- Simulate physical behavior (e.g. pressure, temperature, level, flow, etc.)
- Include safety interlocks (emergency stop, overpressure, overtemp, etc.)
- Include either batch logic or continuous control logic
- Use deterministic state transitions.
- Use states where appropriate (idle, running, stopped, emergency)
- Define a clear cause-and-effect matrix.
- Link one or multiple causes to one or multiple effects
- I.e. actuators and valves can act upon sensor state
- Have a clearly defined system (e.g. reactor, ship engine, boiler, etc.)
- Output all values from the `cycle()` function every 100 ms

The `cycle()` function will be executed once every 100 milliseconds in a loop.

Do not wrap code in triple backticks.  
Output only valid Lua code.  
Avoid floating-point math unless rounded to integer before output.

Here is the simulation template and function library. Remember to implement the required 
functions as the Lua code is self contained.
Think step by step when planning the simulation logic.
Dont use ```for output formating, just plain text.
"""

def tag_prompt(lua_code):
    return f"""
Here is a Lua script for an industrial simulation:

{lua_code}

Now generate a corresponding [Tags] section in INI format for Modbus mapping.
Use this format:
[Tags]
; Format: tag_name,modbus_address,type
; Types: 0=Coil, 1=DiscreteInput, 2=HoldingRegister, 3=InputRegister

Output only the INI-formatted content. Skip ```. 
"""

# Simulation tempalte file
def load_template(filename="simulation_template.lua"):
    if not os.path.exists(filename):
        print(f"[!] Template file not found: {filename}")
        exit(1)
    with open(filename, "r") as f:
        return f.read()

# Request Lua simulation from GPT
def get_lua_script(user_sim_idea, template_code):
    full_prompt = base_prompt + "\nSimulation Idea: " + user_sim_idea.strip() + "\n\n" + template_code
    chat = client.chat.completions.create(
        model="gpt-4o",
        messages=[{"role": "user", "content": full_prompt}],
        temperature=0.7
    )
    return chat.choices[0].message.content.strip()


def save_to_file(filename, content):
    with open(filename, "w") as f:
        f.write(content)
    print(f"Saved to {filename}")

# Generate tags for OPC UA server
def get_tag_map(lua_code):
    chat = client.chat.completions.create(
        model="gpt-4o",
        messages=[{"role": "user", "content": tag_prompt(lua_code)}],
        temperature=0.5
    )
    return chat.choices[0].message.content.strip()


def main():
    template_code = load_template()
    print("===NTNU Specialization Project - ICS Testbed Generator ===")
    user_sim_idea = input("Describe the simulation you want:\n")

    print("Generating Lua simulation...")
    lua_code = get_lua_script(user_sim_idea, template_code)
    save_to_file("simulation.lua", lua_code)

    print("Generating Modbus tag map...")
    ini_tags = get_tag_map(lua_code)
    save_to_file("tags.ini", ini_tags)

    print("Done! Run Simulation with runSimplePLC or copy-paste manually")

if __name__ == "__main__":
    main()
